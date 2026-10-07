/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The request worker: the thread that runs the GPU node's work on the hardware.
 *
 * The session operations only queue.  The worker publishes the node, then
 * takes the queued requests and the synchronous batches in the order they
 * arrived and runs each to its end on the render engine: the request is
 * written into the session context's ring with its breadcrumbs, submitted
 * through execlists, and its end is found by polling the context status
 * buffer.  One request is in flight at a time.  When told to stop, the worker
 * finishes what is queued and withdraws the node.
 *
 * The worker also owns the hardware context behind each session context of
 * the render engine: a logical ring context over the session's own address
 * space, with a timeline page in the GGTT for its breadcrumbs.
 *
 * A request on the video decode engine that hangs or fails has that engine
 * reset and resumed by the worker, up to a bound of hangs (ws083-p007).
 *
 * XXX: the copy engine is a record only, requests on it fail; the ring never
 * wraps and is rewound when an idle context's ring is nearly full; a request
 * on the render engine that does not complete is failed with no reset and
 * no recovery.
 */

#ifndef DRIVERS_GPU_I915_WORKER_H
#define DRIVERS_GPU_I915_WORKER_H

#include <stdint.h>

struct i915_context;
struct i915_device;
struct i915_engine;
struct i915_ppgtt;
struct i915_session;

/*
 * What a queued synchronous item asks of the worker.
 *
 * A batch runs like a request.  A presentation shows a frame on the panel
 * and a release gives the panel back; both belong to the display window,
 * which the worker enters for the first presentation and leaves for the
 * release.  A backlight item reads or sets the panel's brightness or
 * switches its light (I915_BACKLIGHT_*, display/backlight.h), inside the
 * window only (ws113-p013, ws113-p012); it neither enters nor leaves it.
 */
enum i915_worker_sync_kind {
	I915_WORKER_SYNC_BATCH = 0,
	I915_WORKER_SYNC_PRESENT,
	I915_WORKER_SYNC_PRESENT_BLOB,
	I915_WORKER_SYNC_RELEASE,
	I915_WORKER_SYNC_BACKLIGHT
};

/*
 * Builds the batch that copies a frame into a panel buffer: dst_va is the
 * buffer's address in the presenting session's space.
 */
typedef int (*i915_worker_blit_fn)(void *ctx, uint64_t dst_va, uint32_t width, uint32_t height, uint32_t pitch, uint64_t *batch_va);

/*
 * The frame of a presentation, read by the worker while the caller sleeps.
 *
 * A CPU frame names its pixels; a GPU frame names the context and address
 * space its copy runs in and the builder of that copy.
 */
struct i915_worker_present {
	/* A CPU frame: the pixels, their extent and row pitch, and whether they are BGRA. */
	const uint8_t *pixels;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	int bgra;

	/* A GPU frame: the presenting session's address space, the copy's builder and its context. */
	struct i915_ppgtt *vm;
	i915_worker_blit_fn build;
	void *build_ctx;
	struct i915_context *context;

	/* Nonzero: a frame or the release of the second output, the head, not of the resident output (ws113-p011). */
	int head;
};

int drv_i915_worker_create(struct i915_device *device);
int drv_i915_worker_serve(struct i915_device *device);
void drv_i915_worker_stop(struct i915_device *device);
void drv_i915_worker_wake(struct i915_device *device);
int drv_i915_worker_park(struct i915_device *device, unsigned timeout_ms);
void drv_i915_worker_unpark(struct i915_device *device);
void drv_i915_worker_destroy(struct i915_device *device);

int drv_i915_worker_context_create(struct i915_device *device, struct i915_engine *engine, struct i915_ppgtt *vm, uint32_t sw_id, struct i915_context *context);
void drv_i915_worker_context_destroy(struct i915_device *device, struct i915_context *context);
int drv_i915_worker_context_attach(struct i915_device *device, struct i915_context *context);
int drv_i915_worker_video_state(struct i915_device *device);
void drv_i915_worker_video_reclaim(struct i915_device *device);
void drv_i915_worker_kick(struct i915_engine *engine);
int drv_i915_worker_run_sync(struct i915_device *device, struct i915_context *context, uint64_t batch_va);

int drv_i915_worker_sync_display(struct i915_device *device, enum i915_worker_sync_kind kind, const struct i915_worker_present *present);
void drv_i915_worker_serve_window(struct i915_device *device);
int drv_i915_worker_sync_backlight(struct i915_device *device, int op, uint32_t *value);
int drv_i915_worker_run_batch(struct i915_device *device, struct i915_context *context, uint64_t batch_va);

int drv_i915_worker_engine_reset(struct i915_engine *engine);
int drv_i915_worker_engine_recover(struct i915_engine *engine, struct i915_session *session, int error);
int drv_i915_worker_gt_reset(struct i915_device *device);

#endif
