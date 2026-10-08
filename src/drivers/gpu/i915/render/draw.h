/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Recorded draws and rectangles on the GPU.
 *
 * Every operation of one vkQueueSubmit is written into one batch: each takes
 * a slot of the session's state object for its heaps (heap.h) and appends
 * its commands to the session's batch object, and the batch runs once, to
 * its end, when the submission is complete.  An operation outside a
 * submission runs on its own before it returns.  The kernels live in
 * instruction windows of the session's kernel object, placed once.  The
 * three objects are made on the session's first draw or rectangle and kept
 * until the session closes.
 */

#ifndef DRIVERS_GPU_I915_RENDER_DRAW_H
#define DRIVERS_GPU_I915_RENDER_DRAW_H

#include <stdint.h>

#include "batch.h"
#include "gfx.h"

struct i915_gem_object;
struct i915_gfx_kernels;
struct i915_render_session;

/*
 * What a session's draws and rectangles keep between them.
 *
 * The session owns it from its first draw or rectangle to its close.  The
 * objects are bound into the session's address space.  Between
 * drv_i915_gfx_submit_begin() and drv_i915_gfx_submit_end() the operations
 * of the submission are appended to the batch; the batch runs, and the
 * slots and the batch are reused from their start, when it is full, when an
 * operation needs what the recorded ones wrote, and at the end.  Only one
 * thread records into a session at a time.
 */
struct i915_gfx_session {
	/* The state object of the slots, the batch object and the kernel object of the windows. */
	struct i915_gem_object *state;
	struct i915_gem_object *batch;
	struct i915_gem_object *kernels;

	/* How many draws the session has recorded, for the first-draw log line and the checkpoint. */
	unsigned draws;

	/*
	 * Nonzero between the start and the end of a submission: an operation
	 * is then appended to the batch instead of running on its own.
	 */
	int open;

	/* The next free slot of the state object. */
	uint32_t slot_next;

	/* The batch being written; its dwords are the batch object's. */
	struct i915_gfx_batch cursor;

	/* Where the operation being written started in the batch, to take it back when it fails. */
	unsigned op_start;

	/* How many operations the batch holds that have not run yet. */
	uint32_t ops_pending;

	/*
	 * Nonzero when a rectangle that has not run yet may have written a
	 * buffer: a draw that copies uniform data on the CPU runs the batch
	 * first.
	 */
	int transfer_pending;

	/* When the submission in progress started, for the frame timing. */
	uint64_t submit_start;

	/* The next free instruction window of the kernel object. */
	uint32_t window_next;

	/*
	 * The generation of the windows, from 1.  Running out of windows runs
	 * the batch and starts a new generation, and a kernel placed in an
	 * older generation is placed again.
	 */
	uint32_t window_generation;

	/* The windows of the fill and copy kernels of the rectangles, and their generations. */
	uint32_t fill_window;
	uint32_t fill_generation;
	uint32_t copy_window;
	uint32_t copy_generation;

	/* The window of the multisample resolve kernel, and its generation. */
	uint32_t resolve_window;
	uint32_t resolve_generation;

	/*
	 * The scratch buffer, made on the first draw whose kernel spills: the
	 * per-thread space each stage's part (vertex, pixel, compute, geometry)
	 * has room for and where the part starts (heap.h).  It only grows: a kernel that needs more replaces it
	 * once the recorded operations, which may still point at it, have run.
	 */
	struct i915_gem_object *scratch;
	uint32_t scratch_per_thread[4];
	uint64_t scratch_offset[4];

	/*
	 * The session's frame timing over a window of five seconds, logged
	 * when the window ends (ws075-p026): the submissions, the runs they
	 * took and the operations the runs held, the runs forced by a full
	 * batch (no slot left), the time inside the submissions and in their
	 * runs, in nanoseconds, and when the window started.
	 */
	uint64_t stat_start;
	uint32_t stat_submits;
	uint32_t stat_runs;
	uint32_t stat_ops;
	uint32_t stat_full;
	uint32_t stat_ops_now;
	uint32_t stat_ops_max;
	uint64_t stat_submit_ns;
	uint64_t stat_run_ns;
};

/*
 * Where one operation writes: its slot of the state object and its kernels'
 * instruction window, each as the CPU and as the GPU sees it, and the batch
 * its commands go into.
 *
 * It lives on the stack of the operation being written.
 */
struct i915_gfx_op_space {
	/* The slot. */
	uint8_t *slot;
	uint64_t slot_va;

	/* The instruction window. */
	uint8_t *window;
	uint64_t window_va;

	/* The batch the commands are appended to. */
	struct i915_gfx_batch *batch;
};

struct i915_gfx_session *drv_i915_gfx_session_get(struct i915_render_session *session);
void drv_i915_gfx_session_close(struct i915_render_session *session);
int drv_i915_gfx_submit_begin(struct i915_render_session *session);
int drv_i915_gfx_submit_end(struct i915_render_session *session);
int drv_i915_gfx_window(struct i915_render_session *session, struct i915_gfx_session *work, const void **owner, uint32_t *window, uint32_t *generation, const uint32_t *vs_code, uint32_t vs_bytes, const uint32_t *gs_code, uint32_t gs_bytes, const uint32_t *ps_code, uint32_t ps_bytes, struct i915_gfx_op_space *space);
int drv_i915_gfx_op_begin(struct i915_render_session *session, struct i915_gfx_session *work, struct i915_gfx_op_space *space);
int drv_i915_gfx_op_end(struct i915_render_session *session, struct i915_gfx_session *work, int error);
int drv_i915_gfx_flush(struct i915_render_session *session, struct i915_gfx_session *work);

/*
 * Ends a batch, makes it visible to the GPU and runs it to its end on one
 * of the session's engine contexts (I915_ENGINE_RCS0 for the draws,
 * I915_ENGINE_VCS0 for the video decodes).  Returns the run's error.
 */
int drv_i915_gfx_batch_run(struct i915_render_session *session, struct i915_gfx_batch *batch, uint64_t batch_va, unsigned engine);
int drv_i915_gfx_scratch(struct i915_render_session *session, struct i915_gfx_session *work, struct i915_gfx_kernels *kernels);
struct i915_gem_object;
int drv_i915_gfx_object_create(struct i915_render_session *session, uint64_t bytes, struct i915_gem_object **result);
void drv_i915_gfx_object_destroy(struct i915_render_session *session, struct i915_gem_object *object);
int drv_i915_gfx_draw(struct i915_render_session *session, const struct i915_gfx_draw_state *state, const struct i915_gfx_draw_args *args);

#endif
