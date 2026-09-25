/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GPU sessions (see session.h).
 *
 * Open creates the session's address space and one context per engine
 * record; the render engine's context is a logical ring context the request
 * worker owns, the copy engine's is a record only.  Close gives everything
 * back, except that a quarantined session's address space stays with the
 * device until a checked reset proves the GPU no longer names it.
 */

#include "i915.h"
#include "memory.h"
#include "ppgtt.h"
#include "request-queue.h"
#include "session.h"
#include "worker.h"
#include "display/display.h"
#include "render/render.h"
#include <kern/kcrt.h>

#include <drivers/gpu/gpu.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "intel/gt-regs.h"

/* Software context ids stay below the idle marker the context status buffer uses. */
#define I915_CONTEXT_ID_MODULUS		(GEN12_IDLE_CTX_ID - 1U)

static int i915_open(void *opaque, void **result);
static void i915_close(void *opaque, void *private_session);
static int i915_get_info(void *opaque, void *private_session, struct gpu_info *info);
static void i915_session_contexts_destroy(struct i915_device *device, struct i915_session *session);
static void i915_session_batches_destroy(struct i915_device *device, struct i915_session *session);

/*
 * Binds the session operations into the GPU node's operation table.
 */
void
drv_i915_session_bind_ops(
	struct drv_gpu_ops *ops)
{
	/* Opening, closing and describing the node. */
	ops->open = i915_open;
	ops->close = i915_close;
	ops->get_info = i915_get_info;
}

/*
 * Maps a submission timeline to an engine record.
 *
 * Zero names the copy engine record, one and two the render engine record:
 * the Vulkan client numbers the capset's one queue timeline 1.  Any other
 * timeline is EINVAL.
 */
int
drv_i915_engine_for_timeline(
	struct i915_device *device,
	uint32_t timeline,
	struct i915_engine **engine)
{
	/* Nothing is named for a refused timeline. */
	*engine = NULL;

	/*
	 * XXX: the copy engine record is a record only; nothing runs on it, so
	 * the client's queue timeline is served by the render engine.
	 */
	if (timeline == I915_TIMELINE_BCS0) {
		*engine = &device->engines[I915_ENGINE_RCS0];
		return 0;
	}

	/* Picks the record the remaining timelines name. */
	if (timeline == I915_TIMELINE_DEFAULT) {
		*engine = &device->engines[I915_ENGINE_BCS0];
	} else if (timeline == I915_TIMELINE_RCS0) {
		*engine = &device->engines[I915_ENGINE_RCS0];
	} else {
		return EINVAL;
	}

	/* Succeeded: the engine record was filled when the node was published. */
	return 0;
}

/* Opens one session with its own private address space and contexts. */
static int
i915_open(
	void *opaque,
	void **result)
{
	struct i915_device *device;
	struct i915_session *session;
	uint32_t sw_id;
	unsigned index;
	int error;

	/* A failed open transfers nothing to the caller. */
	device = opaque;
	*result = NULL;

	/* Allocates the session state. */
	session = kern_calloc(1U, sizeof(*session));
	if (session == NULL)
		return ENOMEM;

	/* Creates the identifier and the address space under the device mutex. */
	mutex_lock(&device->mutex);

	/* A faulted device refuses new sessions until it is reset. */
	if (device->failed != 0U) {
		mutex_unlock(&device->mutex);
		kern_free(session);
		return ENODEV;
	}

	/* Identifiers are never reused, so the log lines stay unambiguous. */
	if (device->next_session == 0U) {
		mutex_unlock(&device->mutex);
		kern_free(session);
		return EOVERFLOW;
	}

	/* Allocates the address space on its own, so quarantine can keep it past close. */
	session->vm = kern_calloc(1U, sizeof(*session->vm));
	if (session->vm == NULL) {
		mutex_unlock(&device->mutex);
		kern_free(session);
		return ENOMEM;
	}

	/* Builds the empty address space every object binds into at creation. */
	error = drv_i915_ppgtt_create(session->vm);
	if (error != 0) {
		mutex_unlock(&device->mutex);
		kern_free(session->vm);
		kern_free(session);
		return error;
	}

	/* Numbers the session and names it as the owner of its address space. */
	session->device = device;
	session->identifier = device->next_session;
	device->next_session++;
	session->next_slot = 1U;
	session->vm->owner = session->identifier;

	/* Creates one context per engine record, so either record can run the session's work. */
	sw_id = (session->identifier % (I915_CONTEXT_ID_MODULUS - 1U)) + 1U;
	for (index = 0U; index < I915_ENGINE_COUNT; index++) {
		/* Creates the context the request worker runs this record's requests in. */
		error = drv_i915_worker_context_create(
			device,
			&device->engines[index],
			session->vm,
			sw_id,
			&session->contexts[index]);
		if (error != 0) {
			i915_session_contexts_destroy(device, session);
			drv_i915_ppgtt_destroy(session->vm);
			mutex_unlock(&device->mutex);
			kern_free(session->vm);
			kern_free(session);
			return error;
		}
	}

	/* Opens the executor session that wraps this session's address space and lifetime. */
	if (device->vk != NULL) {
		error = drv_i915_render_open(device->vk, session, &session->vk);
		if (error != 0) {
			i915_session_contexts_destroy(device, session);
			drv_i915_ppgtt_destroy(session->vm);
			mutex_unlock(&device->mutex);
			kern_free(session->vm);
			kern_free(session);
			return error;
		}
	}

	mutex_unlock(&device->mutex);

	*result = session;

	/* Succeeded: the GPU core owns the session until close. */
	return 0;
}

/* Closes a session after the GPU core drained its callbacks and retired its resources. */
static void
i915_close(
	void *opaque,
	void *private_session)
{
	struct i915_device *device;
	struct i915_session *session;

	device = opaque;
	session = private_session;

	/* A display lease the session still holds is given back (the panel stops) before anything else goes. */
	drv_i915_display_session_close(device, session);

	/* Closes the executor session first; it is software state over this session. */
	if (session->vk != NULL) {
		drv_i915_render_close(session->vk);
		session->vk = NULL;
	}

	/* Releases the batches, the contexts and the address space under the device mutex. */
	mutex_lock(&device->mutex);

	/*
	 * The batch pool and the contexts go first.  The hardware context of a
	 * quarantined session is released like any other: its image is a GT
	 * object, not a session object the quarantine could retain.
	 */
	i915_session_batches_destroy(device, session);
	i915_session_contexts_destroy(device, session);

	/* A quarantined session's tables stay device-owned until the checked reset frees them. */
	if (session->quarantined == 0U) {
		drv_i915_ppgtt_destroy(session->vm);
		kern_free(session->vm);
	} else {
		session->vm->next = device->quarantined_vms;
		device->quarantined_vms = session->vm;
		kern_logf("i915: session %u closed while quarantined; tables retained\n", session->identifier);
	}

	mutex_unlock(&device->mutex);

	kern_free(session);
}

/* Describes the node to userspace. */
static int
i915_get_info(
	void *opaque,
	void *private_session,
	struct gpu_info *info)
{
	struct i915_device *device;

	UNUSED_PARAMETER(private_session);

	device = opaque;

	/* Describes the capabilities the node was registered with and the contiguous objects it allocates. */
	info->capabilities = device->gpu_ops.capabilities;
	info->max_resources = UINT32_MAX;
	info->max_resource_bytes = I915_MAX_RESOURCE_BYTES;

	/* Names the driver, with the terminating NUL. */
	kern_memset(info->driver_name, 0, sizeof(info->driver_name));
	kern_memcpy(info->driver_name, "i915", 5U);

	/* Succeeded: userspace can identify the native node. */
	return 0;
}

/* Releases every context a session created; the caller holds the device mutex. */
static void
i915_session_contexts_destroy(
	struct i915_device *device,
	struct i915_session *session)
{
	unsigned index;

	/* Releases each context that was created; one that never was holds nothing. */
	for (index = 0U; index < I915_ENGINE_COUNT; index++) {
		if (session->contexts[index].created != 0U)
			drv_i915_worker_context_destroy(device, &session->contexts[index]);
	}
}

/* Releases the session's batch pool; a quarantined session's batches are retained. */
static void
i915_session_batches_destroy(
	struct i915_device *device,
	struct i915_session *session)
{
	struct i915_gem_object *object;

	/* Every batch has retired, because the GPU core drained the session first. */
	while (session->batches != NULL) {
		object = session->batches;
		session->batches = object->session_next;

		/* The GPU may still name a quarantined batch; the checked reset frees it. */
		if (session->quarantined != 0U) {
			object->quarantined = 1U;
		} else {
			drv_i915_gem_unbind_vm(object);
		}

		/* Frees the batch, or leaves a quarantined one on the registry. */
		drv_i915_gem_destroy(&device->gem, object);
	}

	session->batch_count = 0U;
}
