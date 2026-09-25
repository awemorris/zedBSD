/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GPU commands (see command.h).
 *
 * Streams and markers are validated and queued under the device mutex; the
 * slot is taken and the request queued under the IRQ lock, and the request
 * worker is kicked to run it.  The batch pool reuses a session's idle batch
 * objects and is bounded by the request slots.
 *
 * A stream that is not native is a command for the Vulkan executor: it is
 * decoded and executed at once, and its replies are written into the
 * session blob the stream selects.  A session without an executor treats
 * it as a native stream, which the validation refuses.
 */

#include "command.h"
#include "perf.h"
#include "i915.h"
#include "memory.h"
#include "request-queue.h"
#include "session.h"
#include "worker.h"
#include "render/render.h"
#include "render/transport.h"
#include <kern/kcrt.h>

#include <drivers/gpu/gpu.h>
#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/waitq.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "intel/commands.h"

/* The pool never holds more batch objects than requests can be in flight. */
#define I915_BATCH_POOL_MAX	I915_REQUEST_SLOTS

static int i915_command(void *opaque, void *private_session, const void *buffer, uint32_t bytes);
static int i915_command_submit(void *opaque, void *private_session, const void *buffer, uint32_t bytes, uint32_t flags, uint32_t timeline, struct drv_gpu_completion *completion);
static void i915_command_drain(void *opaque, void *private_session);
static int i915_get_capset(void *opaque, void *private_session, struct gpu_capset *capset);
static int i915_stream_is_foreign(const void *buffer, uint32_t bytes);
static int i915_command_render(struct i915_session *session, const void *buffer, uint32_t bytes);
static int i915_submit_stream(struct i915_device *device, struct i915_session *session, const void *buffer, uint32_t bytes, struct drv_gpu_completion *completion);
static int i915_submit_marker(struct i915_device *device, struct i915_session *session, uint32_t timeline, struct drv_gpu_completion *completion);
static int i915_batch_acquire(struct i915_device *device, struct i915_session *session, uint32_t bytes, struct i915_gem_object **result);
static struct i915_gem_object *i915_session_object(struct i915_session *session, uint64_t handle);

/*
 * The queued-command operations of the node: asynchronous submission with a
 * completion, and the drain that waits for every completion of a session.
 */
static const struct drv_gpu_command_ops i915_command_ops = {
	i915_command_submit,
	i915_command_drain
};

/*
 * Binds the command, capset and queued-command operations into the GPU node's operation table.
 */
void
drv_i915_command_bind_ops(
	struct drv_gpu_ops *ops)
{
	/* Synchronous streams and the executor's capset. */
	ops->command = i915_command;
	ops->get_capset = i915_get_capset;

	/* Asynchronous streams, markers and the drain. */
	ops->commands = &i915_command_ops;
}

/*
 * Validates a native command stream and locates its relocations and batch.
 *
 * Only version 1 streams for the render or copy engine with no flags are
 * accepted; the byte count must match the header exactly, the batch must end
 * in MI_BATCH_BUFFER_END, and every relocation must patch a 64-bit address
 * inside the batch.  Returns EINVAL for any other shape; nothing of a
 * rejected stream is reported.
 */
int
drv_i915_stream_parse(
	const void *buffer,
	uint32_t bytes,
	struct i915_stream *stream)
{
	const uint8_t *header;
	uint32_t magic;
	uint32_t version;
	uint32_t flags;
	uint32_t reserved_low;
	uint32_t reserved_high;
	uint32_t expected;
	uint32_t index;
	uint32_t dword_offset;

	/* Nothing of a rejected stream is reported. */
	kern_memset(stream, 0, sizeof(*stream));
	if (bytes < I915_STREAM_HEADER_BYTES)
		return EINVAL;

	/* Decodes the header's little-endian words at their fixed offsets. */
	header = buffer;
	kern_memcpy(&magic, header, 4U);
	kern_memcpy(&version, header + 4U, 4U);
	kern_memcpy(&stream->engine, header + 8U, 4U);
	kern_memcpy(&stream->relocation_count, header + 12U, 4U);
	kern_memcpy(&stream->batch_dwords, header + 16U, 4U);
	kern_memcpy(&flags, header + 20U, 4U);
	kern_memcpy(&reserved_low, header + 24U, 4U);
	kern_memcpy(&reserved_high, header + 28U, 4U);

	/* Accepts only version 1 native streams. */
	if (magic != I915_STREAM_MAGIC || version != I915_STREAM_VERSION)
		return EINVAL;

	/* Accepts only the two engines a stream may name. */
	if (stream->engine != I915_STREAM_ENGINE_RCS0 && stream->engine != I915_STREAM_ENGINE_BCS0)
		return EINVAL;

	/* Refuses flags and reserved words that are not zero. */
	if (flags != 0U ||
	    reserved_low != 0U ||
	    reserved_high != 0U)
		return EINVAL;

	/* Bounds the relocations so the copies below stay within one batch object. */
	if (stream->relocation_count > I915_STREAM_MAX_RELOCATIONS)
		return EINVAL;

	/* Bounds the batch the same way; an empty batch is refused. */
	if (stream->batch_dwords == 0U || stream->batch_dwords > I915_STREAM_MAX_DWORDS)
		return EINVAL;

	/* The byte count must match the header exactly; no trailing bytes are allowed. */
	expected = I915_STREAM_HEADER_BYTES +
	    stream->relocation_count * I915_STREAM_RELOCATION_BYTES +
	    stream->batch_dwords * 4U;
	if (bytes != expected)
		return EINVAL;

	/* The relocation table follows the header, and the batch follows the table. */
	stream->relocations = header + I915_STREAM_HEADER_BYTES;
	stream->batch = (const uint32_t *)(header + I915_STREAM_HEADER_BYTES + stream->relocation_count * I915_STREAM_RELOCATION_BYTES);

	/* A batch must end, so the engine returns to the ring. */
	if (stream->batch[stream->batch_dwords - 1U] != MI_BATCH_BUFFER_END)
		return EINVAL;

	/* Checks that every relocation patches a 64-bit address inside the batch. */
	for (index = 0U; index < stream->relocation_count; index++) {
		kern_memcpy(&dword_offset, stream->relocations + index * I915_STREAM_RELOCATION_BYTES, 4U);
		kern_memcpy(&reserved_low, stream->relocations + index * I915_STREAM_RELOCATION_BYTES + 4U, 4U);

		/* Refuses a relocation whose reserved word is not zero. */
		if (reserved_low != 0U)
			return EINVAL;

		/* Refuses a relocation whose second dword would fall outside the batch. */
		if (dword_offset + 1U >= stream->batch_dwords)
			return EINVAL;
	}

	/* Succeeded: the stream has a sound shape; handles are resolved at submission. */
	return 0;
}

/* Accepts a synchronous native stream; receipt does not wait for execution. */
static int
i915_command(
	void *opaque,
	void *private_session,
	const void *buffer,
	uint32_t bytes)
{
	struct i915_device *device;
	struct i915_session *session;
	int foreign;
	int error;

	device = opaque;
	session = private_session;

	/* Hands a stream that is not a native batch to the executor, which runs it at once. */
	foreign = i915_stream_is_foreign(buffer, bytes);
	if (foreign != 0 && session->vk != NULL) {
		error = i915_command_render(session, buffer, bytes);
		if (error != 0)
			return error;

		/* Succeeded: the executor decoded and executed the stream. */
		return 0;
	}

	/* Validates, copies and queues the stream under the device mutex. */
	mutex_lock(&device->mutex);

	error = i915_submit_stream(device, session, buffer, bytes, NULL);

	mutex_unlock(&device->mutex);

	/* Reports a malformed stream or a full queue. */
	if (error != 0)
		return error;

	/* Succeeded: the engine will run the batch; no completion is reported. */
	return 0;
}

/* Accepts an asynchronous native stream or an empty marker with a completion. */
static int
i915_command_submit(
	void *opaque,
	void *private_session,
	const void *buffer,
	uint32_t bytes,
	uint32_t flags,
	uint32_t timeline,
	struct drv_gpu_completion *completion)
{
	struct i915_device *device;
	struct i915_session *session;
	int foreign;
	int error;

	/* The flag that names a marker is the core's; the timeline picks the engine record. */
	UNUSED_PARAMETER(flags);

	/* Every accepted submission owes exactly one completion. */
	device = opaque;
	session = private_session;
	if (completion == NULL)
		return EINVAL;

	/*
	 * Hands a stream that is not a native batch to the executor.  It
	 * completes as it decodes, so its completion is delivered at once; a
	 * refused stream retains no completion.
	 */
	foreign = i915_stream_is_foreign(buffer, bytes);
	if (foreign != 0 && session->vk != NULL) {
		error = i915_command_render(session, buffer, bytes);
		if (error != 0)
			return error;

		/* Succeeded: the stream is executed and its completion delivered. */
		drv_gpu_complete(completion, 0);
		return 0;
	}

	/* Queues streams and markers along the same serialized submission path. */
	mutex_lock(&device->mutex);

	/* An empty stream is a marker on the selected timeline. */
	if (bytes == 0U) {
		error = i915_submit_marker(device, session, timeline, completion);
	} else {
		error = i915_submit_stream(device, session, buffer, bytes, completion);
	}

	mutex_unlock(&device->mutex);

	/* A refused submission retains no callback. */
	if (error != 0)
		return error;

	/* Succeeded: the completion is delivered when the request retires or fails. */
	return 0;
}

/* Waits until every request of the session retired and delivered its callback. */
static void
i915_command_drain(
	void *opaque,
	void *private_session)
{
	struct i915_device *device;
	struct i915_session *session;
	uint64_t observed;
	unsigned long irq;

	device = opaque;
	session = private_session;

	/* Sleeps until the pending count, dropped only after each callback returned, reaches zero. */
	irq = spin_lock_irqsave(&device->irq_lock);

	while (session->pending_requests != 0U) {
		observed = waitq_sequence(&device->retire_waitq);
		(void)waitq_sleep(&device->retire_waitq, &device->irq_lock, observed, 0U, 0U);
	}

	spin_unlock_irqrestore(&device->irq_lock, irq);
}

/* Reports the executor's capset, so the Vulkan client accepts the node as a backend. */
static int
i915_get_capset(
	void *opaque,
	void *private_session,
	struct gpu_capset *capset)
{
	struct i915_device *device;
	int error;

	UNUSED_PARAMETER(private_session);

	/* The capset belongs to the device's executor, not to any session. */
	device = opaque;

	/* A device without the executor reports no capset. */
	if (device->vk == NULL)
		return ENOTSUP;

	/* Copies the executor's capset; it must fit the requested capacity. */
	error = drv_i915_render_get_capset(device->vk, capset);
	if (error != 0)
		return error;

	/* Succeeded: the client can open the node as a Vulkan backend. */
	return 0;
}

/* Reports nonzero for a stream that is not a native batch: a Vulkan command. */
static int
i915_stream_is_foreign(
	const void *buffer,
	uint32_t bytes)
{
	uint32_t magic;

	/* A stream too short to carry a magic is left to the native validation. */
	if (bytes < 4U)
		return 0;

	/* Reads the stream's first word. */
	kern_memcpy(&magic, buffer, 4U);

	/* A native stream starts with the native magic. */
	if (magic == I915_STREAM_MAGIC)
		return 0;

	/* Any other first word starts a Vulkan command. */
	return 1;
}

/*
 * Executes a Vulkan command stream on the session's executor.
 *
 * The replies go to the session blob the stream selects up front; a stream
 * that selects none replies nowhere.
 */
static int
i915_command_render(
	struct i915_session *session,
	const void *buffer,
	uint32_t bytes)
{
	void *reply;
	size_t reply_bytes;
	size_t *reply_capacity;
	int error;

	/* Resolves the reply region the stream selects. */
	reply = drv_i915_render_transport_reply(session, buffer, bytes, &reply_bytes);

	/* A stream without a reply region is given no capacity to report back into. */
	reply_capacity = NULL;
	if (reply != NULL)
		reply_capacity = &reply_bytes;

	/* Decodes and executes the stream. */
	error = drv_i915_render_execute(session->vk, buffer, bytes, reply, reply_capacity);
	if (error != 0)
		return error;

	/* Succeeded: every command of the stream was executed and its replies published. */
	return 0;
}

/* Validates a stream, builds its batch object and queues the request; the caller holds the device mutex. */
static int
i915_submit_stream(
	struct i915_device *device,
	struct i915_session *session,
	const void *buffer,
	uint32_t bytes,
	struct drv_gpu_completion *completion)
{
	struct i915_stream stream;
	struct i915_engine *engine;
	struct i915_gem_object *batch;
	struct i915_gem_object *target;
	struct i915_request *request;
	uint32_t *dwords;
	uint32_t index;
	uint32_t dword_offset;
	uint64_t handle;
	unsigned long irq;
	int error;

	/* Reports shape errors before any object is touched. */
	error = drv_i915_stream_parse(buffer, bytes, &stream);
	if (error != 0)
		return error;

	/* A quarantined session or a faulted device runs nothing new. */
	if (session->quarantined != 0U || device->failed != 0U)
		return ENODEV;

	/* Picks the engine record the header names; the copy engine unless it names the render engine. */
	engine = &device->engines[I915_ENGINE_BCS0];
	if (stream.engine == I915_STREAM_ENGINE_RCS0)
		engine = &device->engines[I915_ENGINE_RCS0];

	/* Takes a batch object from the session pool, or creates one. */
	error = i915_batch_acquire(device, session, stream.batch_dwords * 4U, &batch);
	if (error != 0)
		return error;

	/* Copies the batch: it is never executed from the caller's buffer. */
	dwords = batch->address;
	kern_memcpy(dwords, stream.batch, (size_t)stream.batch_dwords * 4U);

	/* Writes each relocated object's 64-bit address into the copy. */
	for (index = 0U; index < stream.relocation_count; index++) {
		kern_memcpy(&dword_offset, stream.relocations + index * I915_STREAM_RELOCATION_BYTES, 4U);
		kern_memcpy(&handle, stream.relocations + index * I915_STREAM_RELOCATION_BYTES + 8U, 8U);

		/* Resolves the handle; an unknown one gives the batch back to the pool. */
		target = i915_session_object(session, handle);
		if (target == NULL) {
			batch->busy = 0U;
			return EINVAL;
		}

		dwords[dword_offset] = (uint32_t)target->va;
		dwords[dword_offset + 1U] = (uint32_t)(target->va >> 32);
	}

	/* Makes the copied batch visible before the request can be run. */
	kern_io_write_barrier();

	/* Takes a slot, names the batch and hands the request to the worker. */
	irq = spin_lock_irqsave(&device->irq_lock);

	error = drv_i915_request_alloc(engine, session, completion, &request);
	if (error != 0) {
		batch->busy = 0U;
		spin_unlock_irqrestore(&device->irq_lock, irq);
		return error;
	}

	request->context = &session->contexts[engine->index];
	request->batch = batch;
	request->batch_va = batch->va;
	drv_i915_request_queue(engine, request);
	drv_i915_worker_kick(engine);

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded: the batch runs when the worker reaches it. */
	return 0;
}

/* Queues a marker request without a batch; the caller holds the device mutex. */
static int
i915_submit_marker(
	struct i915_device *device,
	struct i915_session *session,
	uint32_t timeline,
	struct drv_gpu_completion *completion)
{
	struct i915_engine *engine;
	struct i915_request *request;
	unsigned long irq;
	int error;

	/* Finds the engine record whose ordering the marker observes. */
	error = drv_i915_engine_for_timeline(device, timeline, &engine);
	if (error != 0)
		return error;

	/* A quarantined session or a faulted device runs nothing new. */
	if (session->quarantined != 0U || device->failed != 0U)
		return ENODEV;

	/* Takes a slot and hands the marker to the worker. */
	irq = spin_lock_irqsave(&device->irq_lock);

	error = drv_i915_request_alloc(engine, session, completion, &request);
	if (error != 0) {
		spin_unlock_irqrestore(&device->irq_lock, irq);
		return error;
	}

	/* A marker only writes its breadcrumbs. */
	request->context = &session->contexts[engine->index];
	request->queued_at = drv_i915_perf_now();
	drv_i915_request_queue(engine, request);
	drv_i915_worker_kick(engine);

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded: the completion follows every earlier request on this engine record. */
	return 0;
}

/* Finds or creates an idle batch object of at least the given size; the caller holds the device mutex. */
static int
i915_batch_acquire(
	struct i915_device *device,
	struct i915_session *session,
	uint32_t bytes,
	struct i915_gem_object **result)
{
	struct i915_gem_object *object;
	unsigned long irq;
	int error;

	/* Nothing is handed out on a failure. */
	*result = NULL;

	/* Looks for a pooled object that is idle and large enough, and claims it. */
	irq = spin_lock_irqsave(&device->irq_lock);

	/* The busy flag is cleared by retirement under the IRQ lock. */
	object = session->batches;
	while (object != NULL) {
		if (object->busy == 0U && object->bytes >= bytes)
			break;

		object = object->session_next;
	}

	/* A found object is marked busy before the lock goes. */
	if (object != NULL)
		object->busy = 1U;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Hands a reused object back at once. */
	if (object != NULL) {
		*result = object;
		return 0;
	}

	/* The pool is bounded by the request slots, so it never grows without limit. */
	if (session->batch_count >= I915_BATCH_POOL_MAX)
		return EAGAIN;

	/* Creates a new batch object. */
	error = drv_i915_gem_create(&device->gem, bytes, &object);
	if (error != 0)
		return error;

	/* Binds it into the session's address space like a resource. */
	error = drv_i915_gem_bind_vm(session->vm, object);
	if (error != 0) {
		drv_i915_gem_destroy(&device->gem, object);
		return error;
	}

	/* Adds the claimed object to the pool. */
	object->busy = 1U;
	object->session_next = session->batches;
	session->batches = object;
	session->batch_count++;
	*result = object;

	/* Succeeded: the caller fills the batch before queuing it. */
	return 0;
}

/* Resolves a public resource handle to one of the session's objects, or NULL. */
static struct i915_gem_object *
i915_session_object(
	struct i915_session *session,
	uint64_t handle)
{
	struct i915_gem_object *object;

	/* Scans the list; it is short, and relocation resolution needs nothing faster. */
	object = session->objects;
	while (object != NULL && object->handle != handle)
		object = object->session_next;

	/* Reports the object, or NULL for a handle the session does not own. */
	return object;
}
