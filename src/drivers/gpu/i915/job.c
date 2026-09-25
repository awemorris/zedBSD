/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Supervised GPU jobs (see job.h).
 *
 * Every job operation runs under the IRQ lock, like every change of a
 * request slot.  A reserved slot holds the completion but is not queued; the
 * commit queues it as a marker and kicks the request worker.  A normal cancel
 * gives the slot back without a callback; a fault cancel keeps it, as
 * RETAINED, until the session is stopped or isolated.
 */

#include "i915.h"
#include "job.h"
#include "perf.h"
#include "request-queue.h"
#include "session.h"
#include "worker.h"

#include <drivers/gpu/gpu.h>
#include <kern/lock.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

static int i915_job_reserve(void *opaque, void *private_session, uint32_t timeline, struct drv_gpu_completion *completion, void **reservation);
static int i915_job_commit(void *opaque, void *private_session, void *reservation, struct drv_gpu_completion *completion);
static int i915_job_cancel(void *opaque, void *private_session, void *reservation, struct drv_gpu_completion *completion, unsigned fault);
static int i915_job_capacity(void *opaque, void *private_session, uint32_t timeline, unsigned *available);
static struct i915_request *i915_reservation_find(struct i915_device *device, void *reservation, struct drv_gpu_completion *completion, struct i915_engine **engine);
static struct i915_request *i915_reservation(struct i915_engine *engine, void *reservation, struct drv_gpu_completion *completion);

/* The supervised-job operations of the node. */
static const struct drv_gpu_job_ops i915_job_ops = {
	i915_job_reserve,
	i915_job_commit,
	i915_job_cancel,
	i915_job_capacity
};

/*
 * Binds the supervised-job operations into the GPU node's operation table.
 */
void
drv_i915_job_bind_ops(
	struct drv_gpu_ops *ops)
{
	/* Reserve, commit, cancel and the capacity snapshot. */
	ops->jobs = &i915_job_ops;
}

/* Reserves a request slot and its callback before any native submission. */
static int
i915_job_reserve(
	void *opaque,
	void *private_session,
	uint32_t timeline,
	struct drv_gpu_completion *completion,
	void **reservation)
{
	struct i915_device *device;
	struct i915_session *session;
	struct i915_engine *engine;
	struct i915_request *request;
	unsigned long irq;
	int error;

	device = opaque;
	session = private_session;

	/* Refuses a reservation with no callback or no place to return the token. */
	if (completion == NULL || reservation == NULL)
		return EINVAL;

	/* Nothing is retained for a refused reservation. */
	*reservation = NULL;

	/* Jobs name an engine record through a nonzero timeline. */
	if (timeline == I915_TIMELINE_DEFAULT)
		return EINVAL;

	/* Finds the engine record the timeline names. */
	error = drv_i915_engine_for_timeline(device, timeline, &engine);
	if (error != 0)
		return error;

	/* Takes the slot under the IRQ lock, like every queue change. */
	irq = spin_lock_irqsave(&device->irq_lock);

	/* A faulted device, a quarantined session or a stopping one admits no new work. */
	if (device->failed != 0U ||
	    session->quarantined != 0U ||
	    session->stopping != 0U) {
		spin_unlock_irqrestore(&device->irq_lock, irq);
		return ENODEV;
	}

	/* Takes a free slot that holds the callback. */
	error = drv_i915_request_alloc(engine, session, completion, &request);
	if (error != 0) {
		spin_unlock_irqrestore(&device->irq_lock, irq);
		return error;
	}

	/*
	 * RESERVED keeps the slot out of the queue until commit; the pending
	 * count keeps drain and stop waiting for its callback.
	 */
	request->context = &session->contexts[engine->index];
	request->state = I915_REQUEST_RESERVED;
	request->supervised = 1U;
	session->pending_requests++;
	*reservation = request;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded: the GPU core supervises this reservation until commit or cancel. */
	return 0;
}

/* Publishes a reserved slot as a marker request. */
static int
i915_job_commit(
	void *opaque,
	void *private_session,
	void *reservation,
	struct drv_gpu_completion *completion)
{
	struct i915_device *device;
	struct i915_session *session;
	struct i915_engine *engine;
	struct i915_request *request;
	unsigned long irq;

	device = opaque;
	session = private_session;

	/* Refuses a commit with no token or no callback. */
	if (reservation == NULL || completion == NULL)
		return EINVAL;

	/* Resolves the token and queues the marker under the IRQ lock. */
	irq = spin_lock_irqsave(&device->irq_lock);

	/* Resolves the token against the engine records' slots without dereferencing it first. */
	request = i915_reservation_find(device, reservation, completion, &engine);

	/* A stale or already published token cannot enter the queue. */
	if (request == NULL ||
	    request->state != I915_REQUEST_RESERVED ||
	    request->session != session) {
		spin_unlock_irqrestore(&device->irq_lock, irq);
		return ESTALE;
	}

	/* No commit follows a fault or an isolation. */
	if (device->failed != 0U || session->quarantined != 0U) {
		spin_unlock_irqrestore(&device->irq_lock, irq);
		return ENODEV;
	}

	/*
	 * The marker runs like any request.  The reservation's pending count is
	 * given back because queuing counts the request again.
	 */
	request->state = I915_REQUEST_QUEUED;
	session->pending_requests--;
	request->queued_at = drv_i915_perf_now();
	drv_i915_request_queue(engine, request);
	drv_i915_worker_kick(engine);

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded: the worker reports the marker through the reserved callback. */
	return 0;
}

/* Withdraws a reservation; a fault cancel keeps the callback for the GPU core. */
static int
i915_job_cancel(
	void *opaque,
	void *private_session,
	void *reservation,
	struct drv_gpu_completion *completion,
	unsigned fault)
{
	struct i915_device *device;
	struct i915_session *session;
	struct i915_engine *engine;
	struct i915_request *request;
	unsigned long irq;

	device = opaque;
	session = private_session;

	/* Refuses a cancel with no token, no callback, or an unknown kind. */
	if (reservation == NULL ||
	    completion == NULL ||
	    fault > 1U)
		return EINVAL;

	/* Resolves the token and withdraws it under the IRQ lock. */
	irq = spin_lock_irqsave(&device->irq_lock);

	/* The token must name a slot that still holds this callback. */
	request = i915_reservation_find(device, reservation, completion, &engine);
	if (request == NULL || request->session != session) {
		spin_unlock_irqrestore(&device->irq_lock, irq);
		return ESTALE;
	}

	/*
	 * A definite rollback needs an unpublished slot; a fault cancel may also
	 * end queued or running work, but never a free slot.
	 */
	if (request->state != I915_REQUEST_RESERVED) {
		if (fault == 0U || request->state == I915_REQUEST_FREE) {
			spin_unlock_irqrestore(&device->irq_lock, irq);
			return ESTALE;
		}
	}

	/* An uncertain outcome keeps the slot and its callback until the session is stopped or isolated. */
	if (fault != 0U) {
		if (request->state == I915_REQUEST_RESERVED)
			request->state = I915_REQUEST_RETAINED;

		spin_unlock_irqrestore(&device->irq_lock, irq);
		return 0;
	}

	/* A definite nonacceptance returns the slot with no future callback. */
	session->pending_requests--;
	request->completion = NULL;
	drv_i915_request_release(engine, request);

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Tells the GPU core that the freed slot is capacity for the next reservation. */
	if (device->gpu != NULL)
		drv_gpu_capacity_changed(device->gpu);

	/* Succeeded: the reservation is gone without inventing a completion. */
	return 0;
}

/* Reports the free request slots of the engine record a timeline names. */
static int
i915_job_capacity(
	void *opaque,
	void *private_session,
	uint32_t timeline,
	unsigned *available)
{
	struct i915_device *device;
	struct i915_engine *engine;
	unsigned index;
	unsigned long irq;
	int error;

	UNUSED_PARAMETER(private_session);

	device = opaque;

	/* Refuses a snapshot with no place to report it. */
	if (available == NULL)
		return EINVAL;

	/* A refused snapshot reports no capacity. */
	*available = 0U;

	/* Jobs name an engine record through a nonzero timeline. */
	if (timeline == I915_TIMELINE_DEFAULT)
		return EINVAL;

	/* Finds the engine record the timeline names. */
	error = drv_i915_engine_for_timeline(device, timeline, &engine);
	if (error != 0)
		return error;

	/* Counts the free slots under the IRQ lock without sleeping. */
	irq = spin_lock_irqsave(&device->irq_lock);

	/* A faulted device has no capacity to report. */
	if (device->failed != 0U) {
		spin_unlock_irqrestore(&device->irq_lock, irq);
		return ENODEV;
	}

	/* Counts every free slot of the record. */
	for (index = 0U; index < I915_REQUEST_SLOTS; index++) {
		if (engine->slots[index].state == I915_REQUEST_FREE)
			(*available)++;
	}

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded: the GPU core combines this with its own admission ledger. */
	return 0;
}

/* Resolves a reservation token on the render record, then on the copy record; the caller holds the IRQ lock. */
static struct i915_request *
i915_reservation_find(
	struct i915_device *device,
	void *reservation,
	struct drv_gpu_completion *completion,
	struct i915_engine **engine)
{
	struct i915_request *request;

	/* Looks among the render record's slots first. */
	*engine = &device->engines[I915_ENGINE_RCS0];
	request = i915_reservation(*engine, reservation, completion);
	if (request != NULL)
		return request;

	/* Then among the copy record's slots. */
	*engine = &device->engines[I915_ENGINE_BCS0];
	request = i915_reservation(*engine, reservation, completion);
	if (request != NULL)
		return request;

	/* Reports a token no slot of either record holds. */
	return NULL;
}

/* Resolves a reservation token to an engine record's slot holding the same callback, or NULL. */
static struct i915_request *
i915_reservation(
	struct i915_engine *engine,
	void *reservation,
	struct drv_gpu_completion *completion)
{
	unsigned index;

	/* Compares foreign addresses only; the token is never dereferenced. */
	for (index = 0U; index < I915_REQUEST_SLOTS; index++) {
		if (reservation != &engine->slots[index])
			continue;

		/* The slot must still hold the callback the token was given with. */
		if (engine->slots[index].completion == completion)
			return &engine->slots[index];
	}

	/* Reports a token this record does not hold. */
	return NULL;
}
