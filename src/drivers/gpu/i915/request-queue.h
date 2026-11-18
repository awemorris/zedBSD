/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The request queue of the GPU node.
 *
 * Sessions hand the node work as requests: a native batch, a marker with no
 * batch, or a job reserved ahead of its commit.  Each request takes a slot of
 * an engine record, waits in that engine's queue, is handed to the request
 * worker (worker.h) that runs it on the hardware, and retires by delivering
 * its completion to the GPU core.  The queue is plain memory; nothing here
 * touches the hardware.
 */

#ifndef DRIVERS_GPU_I915_REQUEST_QUEUE_H
#define DRIVERS_GPU_I915_REQUEST_QUEUE_H

#include <stdint.h>

#include "device-info.h"

struct drv_gpu_completion;
struct i915_device;
struct i915_gem_object;
struct i915_ppgtt;
struct i915_session;

/* The engine records of the node: the render engine, the copy engine and the video decode engine VCS0. */
#define I915_ENGINE_RCS0		0U
#define I915_ENGINE_BCS0		1U
#define I915_ENGINE_VCS0		2U
#define I915_ENGINE_COUNT		3U

/* The engine class of each record, as the engine information names it. */
#define I915_CLASS_RENDER		I915_RENDER_CLASS
#define I915_CLASS_COPY			I915_COPY_ENGINE_CLASS
#define I915_CLASS_VIDEO		I915_VIDEO_DECODE_CLASS

/* How many requests one engine record holds at once, queued or in flight. */
#define I915_REQUEST_SLOTS		32U

/*
 * The states of a request slot.
 *
 * FREE slots may be taken.  QUEUED requests wait in the engine queue, ACTIVE
 * ones were handed to the request worker, DONE ones retired and wait for
 * their completion to be delivered.  RESERVED slots hold a job's completion
 * but were never queued; RETAINED ones are reservations whose fault cancel
 * left the outcome uncertain.
 */
#define I915_REQUEST_FREE		0U
#define I915_REQUEST_QUEUED		1U
#define I915_REQUEST_ACTIVE		2U
#define I915_REQUEST_DONE		3U
#define I915_REQUEST_RESERVED		4U
#define I915_REQUEST_RETAINED		5U

/*
 * One session's context on one engine record.
 *
 * The session embeds one per engine.  It names the session's address space
 * and software context id; the hardware context behind it (the logical ring
 * context the render engine loads) is owned by the request worker, which
 * finds it by this record's address.
 */
struct i915_context {
	/* The engine record the context belongs to. */
	struct i915_engine *engine;

	/* The session's private address space. */
	struct i915_ppgtt *vm;

	/* The software context id the descriptor carries. */
	uint32_t sw_id;

	/* Nonzero between a successful create and the destroy. */
	unsigned created;
};

/*
 * One request: a slot of an engine record.
 *
 * Slots are recycled once the request retired and its completion was
 * delivered.  The device's IRQ lock protects the state, the queue link and
 * the counters of every slot.
 */
struct i915_request {
	/* One of the I915_REQUEST_* states. */
	unsigned state;

	/* The engine record's sequence number, given when the worker takes the request. */
	uint32_t seqno;

	/* The session's context the request runs in, and the session. */
	struct i915_context *context;
	struct i915_session *session;

	/* The GPU core's callback, delivered exactly once; NULL when none is owed. */
	struct drv_gpu_completion *completion;

	/* The batch object and its address in the session's space; NULL for a marker. */
	struct i915_gem_object *batch;
	uint64_t batch_va;

	/* Nonzero for a reserved job the GPU core supervises. */
	unsigned supervised;

	/* How the request ended: 0 or a positive errno. */
	int error;

	/* When the request was queued (drv_i915_perf_now()), for the timing report. */
	uint64_t queued_at;

	/* The engine queue, the worker's run list, or a retired list. */
	struct i915_request *next;
};

/*
 * One engine record of the node.
 *
 * The device embeds one per engine slot.  Publication fills it; it holds the
 * request slots and the queue of requests not yet handed to the request
 * worker.  The device's IRQ lock protects every field after publication.
 */
struct i915_engine {
	/* The device the record belongs to. */
	struct i915_device *device;

	/* The record's index and its engine class. */
	unsigned index;
	unsigned class;

	/* The sequence number the next request is given, and the last one that retired. */
	uint32_t next_seqno;
	uint32_t completed_seqno;

	/* The request slots. */
	struct i915_request slots[I915_REQUEST_SLOTS];

	/* The requests waiting to be handed to the worker, oldest first. */
	struct i915_request *queue_head;
	struct i915_request *queue_tail;

	/*
	 * A request the engine runs outside the worker.  The worker never sets
	 * them, so they stay NULL and zero; the fail path still honours them.
	 */
	struct i915_request *active;
	unsigned hw_active;

	/* Nonzero once publication has filled the record. */
	unsigned initialized;
};

int drv_i915_request_alloc(struct i915_engine *engine, struct i915_session *session, struct drv_gpu_completion *completion, struct i915_request **result);
void drv_i915_request_release(struct i915_engine *engine, struct i915_request *request);
void drv_i915_request_queue(struct i915_engine *engine, struct i915_request *request);
void drv_i915_request_fail(struct i915_engine *engine, struct i915_session *session, int error, struct i915_request **retired);
void drv_i915_request_complete_list(struct i915_engine *engine, struct i915_request *retired);

#endif
