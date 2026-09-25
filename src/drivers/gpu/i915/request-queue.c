/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The request queue of the GPU node (see request-queue.h).
 *
 * Slots are taken and queued under the device's IRQ lock.  The request worker
 * takes queued requests off the engine queue; a retired request delivers its
 * completion outside every driver lock and only then gives its slot back.
 */

#include "i915.h"
#include "request-queue.h"
#include "session.h"
#include <kern/kcrt.h>

#include <drivers/gpu/gpu.h>
#include <kern/lock.h>
#include <kern/waitq.h>

#include <uapi/errno.h>
#include <stddef.h>

static void i915_request_unlink(struct i915_engine *engine, struct i915_request *request);

/*
 * Takes a free slot for a new request.
 *
 * The caller holds the IRQ lock.  Returns EAGAIN when every slot of the
 * engine record is in use.
 */
int
drv_i915_request_alloc(
	struct i915_engine *engine,
	struct i915_session *session,
	struct drv_gpu_completion *completion,
	struct i915_request **result)
{
	struct i915_request *request;
	unsigned index;

	/* No caller receives a slot it does not own. */
	*result = NULL;

	/* Takes the first slot that retired and had its completion delivered. */
	request = NULL;
	for (index = 0U; index < I915_REQUEST_SLOTS; index++) {
		if (engine->slots[index].state == I915_REQUEST_FREE) {
			request = &engine->slots[index];
			break;
		}
	}

	/* Every slot busy means the engine's bounded queue is full. */
	if (request == NULL)
		return EAGAIN;

	/* Starts the slot as a queued request that names its session and callback. */
	kern_memset(request, 0, sizeof(*request));
	request->state = I915_REQUEST_QUEUED;
	request->session = session;
	request->completion = completion;
	*result = request;

	/* Succeeded: the caller fills the request and queues it. */
	return 0;
}

/*
 * Returns a slot that will never be queued.
 *
 * The caller holds the IRQ lock.  A request still linked or active cannot be
 * released.
 */
void
drv_i915_request_release(
	struct i915_engine *engine,
	struct i915_request *request)
{
	UNUSED_PARAMETER(engine);

	/* FREE lets the next allocation take the slot. */
	request->state = I915_REQUEST_FREE;
}

/*
 * Appends a filled request to the engine queue.
 *
 * The caller holds the IRQ lock.  The session counts the request as pending
 * until its completion has been delivered.
 */
void
drv_i915_request_queue(
	struct i915_engine *engine,
	struct i915_request *request)
{
	/* Links the request at the tail: the queue is first in, first out across every session. */
	request->next = NULL;
	if (engine->queue_tail == NULL) {
		engine->queue_head = request;
	} else {
		engine->queue_tail->next = request;
	}

	engine->queue_tail = request;

	/* The pending count keeps drain and stop waiting until the completion has run. */
	if (request->session != NULL)
		request->session->pending_requests++;
}

/*
 * Ends every queued and active request of a session, or of all sessions, with an error.
 *
 * The caller holds the IRQ lock.  The failed requests are chained through
 * their next pointer onto *retired for the caller to complete outside the
 * lock.  Reserved and retained slots are failed too, because they still owe
 * the GPU core a callback.
 */
void
drv_i915_request_fail(
	struct i915_engine *engine,
	struct i915_session *session,
	int error,
	struct i915_request **retired)
{
	struct i915_request *request;
	struct i915_request *next;
	unsigned index;

	/* Unlinks and retires the queued requests without ever running them. */
	request = engine->queue_head;
	while (request != NULL) {
		next = request->next;

		/* Only the named session's requests are failed, or all of them for none. */
		if (session == NULL || request->session == session) {
			i915_request_unlink(engine, request);
			request->state = I915_REQUEST_DONE;
			request->error = error;
			request->next = *retired;
			*retired = request;
		}

		request = next;
	}

	/* Withdraws the active request only when it belongs to the session asked for. */
	request = engine->active;
	if (request != NULL) {
		if (session == NULL || request->session == session) {
			engine->active = NULL;
			engine->hw_active = 0U;
			engine->completed_seqno = request->seqno;
			request->state = I915_REQUEST_DONE;
			request->error = error;
			request->next = *retired;
			*retired = request;
		}
	}

	/* Fails the reserved and retained slots, which still owe the GPU core a callback. */
	for (index = 0U; index < I915_REQUEST_SLOTS; index++) {
		request = &engine->slots[index];

		/* Skips a slot that is neither reserved nor retained. */
		if (request->state != I915_REQUEST_RESERVED) {
			if (request->state != I915_REQUEST_RETAINED)
				continue;
		}

		/* Skips another session's slot. */
		if (session != NULL && request->session != session)
			continue;

		request->state = I915_REQUEST_DONE;
		request->error = error;
		request->next = *retired;
		*retired = request;
	}
}

/*
 * Delivers the completions of retired requests and frees their slots.
 *
 * The caller holds no driver lock: each callback runs unlocked, and the slot
 * is given back under the IRQ lock afterwards.
 */
void
drv_i915_request_complete_list(
	struct i915_engine *engine,
	struct i915_request *retired)
{
	struct i915_device *device;
	struct i915_request *request;
	struct i915_request *next;
	struct drv_gpu_completion *completion;
	int error;
	unsigned freed;

	device = engine->device;
	freed = 0U;

	/* Delivers each outcome, then returns the slot. */
	request = retired;
	while (request != NULL) {
		next = request->next;
		completion = request->completion;
		error = request->error;

		/* The GPU core learns the outcome before the slot can be reused. */
		if (completion != NULL)
			drv_gpu_complete(completion, error);

		/* Returns the slot so a concurrent allocation sees it consistently. */
		spin_lock(&device->irq_lock);

		/* The session's pending count drops only after its callback has run. */
		if (request->session != NULL && request->session->pending_requests != 0U)
			request->session->pending_requests--;

		/* A batch object goes back to its session pool for the next submission. */
		if (request->batch != NULL)
			request->batch->busy = 0U;

		request->batch = NULL;
		request->completion = NULL;
		request->state = I915_REQUEST_FREE;

		spin_unlock(&device->irq_lock);

		freed++;
		request = next;
	}

	/* Wakes drain and capacity waiters, which observe the released slots. */
	if (freed != 0U) {
		waitq_wake_all(&device->retire_waitq);

		/* Tells the GPU core that job capacity came back. */
		if (device->gpu != NULL)
			drv_gpu_capacity_changed(device->gpu);
	}
}

/* Removes a request from the engine queue. */
static void
i915_request_unlink(
	struct i915_engine *engine,
	struct i915_request *request)
{
	struct i915_request **position;

	/* Scans the singly linked queue for the link that names the request. */
	position = &engine->queue_head;
	while (*position != NULL && *position != request)
		position = &(*position)->next;

	/* A request that is not queued has nothing to unlink. */
	if (*position != request)
		return;

	*position = request->next;

	/* Recomputes the tail when the request was the last one. */
	if (engine->queue_tail == request) {
		engine->queue_tail = NULL;
		position = &engine->queue_head;
		while (*position != NULL) {
			engine->queue_tail = *position;
			position = &(*position)->next;
		}
	}

	request->next = NULL;
}
