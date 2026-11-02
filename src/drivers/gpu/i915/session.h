/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GPU sessions: open, close and the device description.
 *
 * A session is one open of the GPU node.  It owns a private address space,
 * one context per engine record, its numbered objects and a pool of batch
 * objects.  Submission timelines name the engine record a job or marker runs
 * on.
 */

#ifndef DRIVERS_GPU_I915_SESSION_H
#define DRIVERS_GPU_I915_SESSION_H

#include <uapi/gpu.h>
#include <uapi/gpu-allocation.h>
#include <uapi/gpu-job.h>
#include <stdint.h>

#include "request-queue.h"

struct drv_gpu_ops;
struct i915_device;
struct i915_gem_object;
struct i915_ppgtt;
struct i915_render_session;

/*
 * What every node offers: storage with CPU copies and mappings, native
 * streams, queued completion, supervised jobs, the capset and blobs, and
 * sharing between the opens of the node, of images and of whole
 * allocations (an exported Vulkan memory, such as a decoded picture).  A node with a panel adds the
 * display and its events when the display operations are bound.
 */
#define I915_CAPABILITIES	(GPU_CAP_RESOURCE | \
				 GPU_CAP_TRANSFER | \
				 GPU_CAP_COMMAND | \
				 GPU_CAP_NOTIFICATION | \
				 GPU_CAP_JOB | \
				 GPU_CAP_JOB_CAPACITY | \
				 GPU_CAP_CAPSET | \
				 GPU_CAP_BLOB | \
				 GPU_CAP_MAPPING | \
				 GPU_CAP_SHARE | \
				 GPU_CAP_ALLOCATION_SHARE)

/*
 * Submission timelines.
 *
 * Zero is the default and names the copy engine record.  The capset
 * declares one queue timeline, which the Vulkan client numbers 1, so
 * timeline 1 names the render engine record; 2 names it as well.
 */
#define I915_TIMELINE_DEFAULT		0U
#define I915_TIMELINE_BCS0		1U
#define I915_TIMELINE_RCS0		2U

/*
 * One open GPU session.
 *
 * The GPU core owns it from open to close.  The address space is a separate
 * allocation so a quarantined session can hand it to the device at close
 * without allocating on a path that cannot fail.  The device mutex protects
 * the object lists and counters; the IRQ lock protects quarantined, stopping
 * and pending_requests.
 */
struct i915_session {
	/* The device the session was opened on. */
	struct i915_device *device;

	/* The session's number, never reused, for the log lines. */
	uint32_t identifier;

	/* The session's private address space. */
	struct i915_ppgtt *vm;

	/* One context per engine record. */
	struct i915_context contexts[I915_ENGINE_COUNT];

	/* The resources and blobs the session created, newest first. */
	struct i915_gem_object *objects;

	/*
	 * The Vulkan executor's side of the session, opened with the session
	 * and closed first at close; NULL when the device has no executor.
	 */
	struct i915_render_session *vk;

	/* The batch pool and how many batch objects it holds. */
	struct i915_gem_object *batches;
	unsigned batch_count;

	/* The slot the next object is numbered with, and how many resources are live. */
	uint32_t next_slot;
	unsigned resources;

	/* Nonzero once isolate quarantined the session: its objects stay device-owned. */
	unsigned quarantined;

	/* Nonzero once the GPU core began to stop the session. */
	unsigned stopping;

	/*
	 * The requests queued, running, reserved, or whose callback has not
	 * returned yet.  Drain and stop wait for it to reach zero.
	 */
	unsigned pending_requests;
};

void drv_i915_session_bind_ops(struct drv_gpu_ops *ops);
int drv_i915_engine_for_timeline(struct i915_device *device, uint32_t timeline, struct i915_engine **engine);

#endif
