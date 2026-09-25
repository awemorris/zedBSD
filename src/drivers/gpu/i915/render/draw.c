/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Recorded draws on the GPU, and the batch of a submission (see draw.h).
 *
 * The pipeline's kernels come from the executor's own compiler and are
 * placed once in an instruction window.  A draw writes its heaps into a
 * slot of the state object and appends its commands to the batch; the batch
 * runs synchronously on the session's render context when the submission
 * ends, or at once for a draw outside a submission.
 */

#include "draw.h"
#include "batch.h"
#include "gfx.h"
#include "heap.h"
#include "internal.h"
#include "state.h"
#include <kern/kcrt.h>

#include "../i915.h"
#include "../memory.h"
#include "../perf.h"
#include "../session.h"
#include "../worker.h"

#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "../intel/genxml.h"

/*
 * The draw checkpoint.
 *
 * The test build defines it to look at what the first draws of a session
 * left in their render target (the pixel census and the image dump the
 * oracle test rebuilds).  It is weak so that a production kernel links
 * without it and the call site is a null test.
 */
extern void drv_i915_gfx_draw_checkpoint(const struct i915_gfx_image *target, unsigned draw) __attribute__((weak));

/* The stages of the scratch buffer's parts, as the index of struct i915_gfx_session's scratch arrays. */
#define I915_DRAW_SCRATCH_VERTEX	0U
#define I915_DRAW_SCRATCH_PIXEL		1U

static int i915_draw_object_create(struct i915_render_session *session, uint64_t bytes, struct i915_gem_object **result);
static int i915_draw_scratch(struct i915_render_session *session, struct i915_gfx_session *work, struct i915_gfx_kernels *kernels);
static int i915_draw_scratch_grow(struct i915_render_session *session, struct i915_gfx_session *work, const struct i915_gfx_kernels *kernels);
static void i915_draw_object_destroy(struct i915_render_session *session, struct i915_gem_object *object);
static int i915_draw_build_batch(struct i915_gfx_batch *batch, const struct i915_gfx_op_space *space, const struct i915_gfx_draw_state *state, const struct i915_gfx_kernels *kernels, const struct i915_gfx_image *target, const struct i915_gfx_image *depth, uint32_t mocs, const struct i915_gfx_draw_args *args);

/*
 * Returns the objects the session's draws and rectangles share, making them
 * on first use.
 *
 * Returns NULL when an object cannot be made.
 * XXX: the objects made before the one that failed are not released.
 */
struct i915_gfx_session *
drv_i915_gfx_session_get(
	struct i915_render_session *session)
{
	struct i915_gfx_session *work;
	int error;

	/* Reuses the objects made by an earlier draw or rectangle. */
	if (session->gfx != NULL)
		return session->gfx;

	/* Allocates the record of the objects. */
	work = kern_calloc(1U, sizeof(*work));
	if (work == NULL)
		return NULL;

	/* Makes the state object of the slots. */
	error = i915_draw_object_create(session, I915_GFX_STATE_BYTES, &work->state);
	if (error != 0) {
		kern_free(work);
		return NULL;
	}

	/* Makes the batch object. */
	error = i915_draw_object_create(session, I915_GFX_BATCH_BYTES, &work->batch);
	if (error != 0) {
		kern_free(work);
		return NULL;
	}

	/* Makes the kernel object of the instruction windows. */
	error = i915_draw_object_create(session, I915_GFX_KERNEL_BYTES, &work->kernels);
	if (error != 0) {
		kern_free(work);
		return NULL;
	}

	/* Starts with an empty batch over the batch object and the first generation of windows. */
	work->cursor.cmds = work->batch->address;
	work->cursor.count = 0U;
	work->cursor.capacity = I915_GFX_BATCH_BYTES / 4U;
	work->cursor.overflow = 0;
	work->window_generation = 1U;

	/* Keeps the objects with the session until it closes. */
	session->gfx = work;

	/* Succeeded: the session has its state, batch and kernel objects. */
	return work;
}

/*
 * Releases what the session's draws and rectangles kept: the state, batch
 * and kernel objects.
 */
void
drv_i915_gfx_session_close(
	struct i915_render_session *session)
{
	struct i915_gfx_session *work;
	struct i915_device *device;

	/* A session that never drew keeps nothing. */
	work = session->gfx;
	if (work == NULL)
		return;

	/* Unbinds and destroys the three objects. */
	device = session->vk->i915;
	mutex_lock(&device->mutex);

	drv_i915_gem_unbind_vm(work->state);
	drv_i915_gem_destroy(&device->gem, work->state);
	drv_i915_gem_unbind_vm(work->batch);
	drv_i915_gem_destroy(&device->gem, work->batch);
	drv_i915_gem_unbind_vm(work->kernels);
	drv_i915_gem_destroy(&device->gem, work->kernels);

	mutex_unlock(&device->mutex);

	/* Destroys the scratch buffer the spilling kernels had. */
	if (work->scratch != NULL)
		i915_draw_object_destroy(session, work->scratch);

	/* Forgets the record, so a later draw makes new objects. */
	kern_free(work);
	session->gfx = NULL;
}

/*
 * Opens the batch a submission's operations are recorded into.
 *
 * Returns ENOMEM when the session's objects cannot be made.
 */
int
drv_i915_gfx_submit_begin(
	struct i915_render_session *session)
{
	struct i915_gfx_session *work;

	/* Takes the session's objects, making them for its first submission. */
	work = drv_i915_gfx_session_get(session);
	if (work == NULL)
		return ENOMEM;

	/*
	 * From here every draw and rectangle is appended to the batch, which
	 * starts empty with every slot free.
	 */
	work->open = 1;
	work->slot_next = 0U;
	work->cursor.count = 0U;
	work->cursor.overflow = 0;
	work->ops_pending = 0U;
	work->transfer_pending = 0;

	/* Notes when the submission started, for the frame timing. */
	work->submit_start = drv_i915_perf_now();

	/* Succeeded: the batch is open. */
	return 0;
}

/*
 * Runs what a submission recorded and closes its batch.
 *
 * Returns 0, or the error of the GPU run.
 */
int
drv_i915_gfx_submit_end(
	struct i915_render_session *session)
{
	struct i915_gfx_session *work;
	struct i915_device *device;
	int error;

	/* A session with no objects recorded nothing. */
	work = session->gfx;
	if (work == NULL)
		return 0;

	/* Runs the operations that have not run yet, then closes the batch. */
	error = drv_i915_gfx_flush(session, work);
	work->open = 0;

	/* Counts the submission's time and logs the timing once a window is over. */
	device = session->vk->i915;
	drv_i915_perf_add(&device->perf, I915_PERF_SUBMIT, work->submit_start);
	drv_i915_perf_report(&device->perf, 0);

	/* Reports a batch that failed on the GPU. */
	if (error != 0)
		return error;

	/* Succeeded: every operation of the submission has run to its end. */
	return 0;
}

/*
 * Finds the instruction window of a pair of kernels, placing them in a new
 * window when they are not in one of the current generation.
 *
 * `owner`, `window` and `generation` are where the caller keeps the place
 * (owner NULL for kernels that belong to the session record itself).  A new
 * window is filled with threads that only end themselves before the kernels
 * are copied in; when every window is taken, what the batch holds runs and a
 * new generation starts from the first window.  Returns 0 with the window in
 * `space`, or the error of that run.
 */
int
drv_i915_gfx_window(
	struct i915_render_session *session,
	struct i915_gfx_session *work,
	const void **owner,
	uint32_t *window,
	uint32_t *generation,
	const uint32_t *vs_code,
	uint32_t vs_bytes,
	const uint32_t *ps_code,
	uint32_t ps_bytes,
	struct i915_gfx_op_space *space)
{
	uint8_t *address;
	int placed;
	int error;

	/* The kernels are in place when the record, the window and the generation still match. */
	placed = 0;
	if (*generation == work->window_generation && *window < work->window_next) {
		placed = 1;
		if (owner != NULL && *owner != (const void *)work)
			placed = 0;
	}

	/* Places the kernels in the next window when they are not in place. */
	if (!placed) {
		/*
		 * Every window is taken: the recorded operations still name
		 * windows, so they run before the windows are used again.
		 */
		if (work->window_next == I915_GFX_KERNEL_WINDOWS) {
			error = drv_i915_gfx_flush(session, work);
			if (error != 0)
				return error;

			work->window_generation++;
			work->window_next = 0U;
		}

		/* Fills the window with threads that end at once, then copies the kernels in. */
		address = (uint8_t *)work->kernels->address + (uint64_t)work->window_next * I915_GFX_INSTRUCTION_BYTES;
		drv_i915_gfx_instruction_heap_clear(address);
		if (vs_code != NULL)
			kern_memcpy(address + I915_GFX_VS_KERNEL, vs_code, vs_bytes);
		if (ps_code != NULL)
			kern_memcpy(address + I915_GFX_PS_KERNEL, ps_code, ps_bytes);

		/* Remembers where the kernels are, for every later operation that runs them. */
		*window = work->window_next;
		*generation = work->window_generation;
		if (owner != NULL)
			*owner = work;
		work->window_next++;
	}

	/* Hands the window to the operation. */
	space->window = (uint8_t *)work->kernels->address + (uint64_t)*window * I915_GFX_INSTRUCTION_BYTES;
	space->window_va = work->kernels->va + (uint64_t)*window * I915_GFX_INSTRUCTION_BYTES;

	/* Succeeded: the kernels are in the window. */
	return 0;
}

/*
 * Takes a slot and batch room for one operation.
 *
 * Inside a submission, what the batch holds runs first when no slot or not
 * enough room for one operation is left; outside one, the operation starts
 * an empty batch of its own.  Returns 0 with the slot and the batch in
 * `space`, or the error of that run.
 */
int
drv_i915_gfx_op_begin(
	struct i915_render_session *session,
	struct i915_gfx_session *work,
	struct i915_gfx_op_space *space)
{
	int full;
	int error;

	/* Notes whether a slot and the room of one operation are left. */
	full = 0;
	if (work->slot_next >= I915_GFX_SLOTS) {
		full = 1;
	} else if (work->cursor.capacity - work->cursor.count < I915_GFX_OP_MAX_DWORDS) {
		full = 1;
	}

	/* Runs what the batch holds when the operation does not fit, or outside a submission. */
	if (full || !work->open) {
		error = drv_i915_gfx_flush(session, work);
		if (error != 0)
			return error;
	}

	/* Hands the next slot and the batch to the operation, which starts here. */
	space->slot = (uint8_t *)work->state->address + (uint64_t)work->slot_next * I915_GFX_SLOT_BYTES;
	space->slot_va = work->state->va + (uint64_t)work->slot_next * I915_GFX_SLOT_BYTES;
	space->batch = &work->cursor;
	work->op_start = work->cursor.count;

	/* Succeeded: the operation may write its slot and its commands. */
	return 0;
}

/*
 * Completes one operation begun by drv_i915_gfx_op_begin().
 *
 * `error` is how writing the operation ended.  A failed operation, or one
 * that did not fit the batch, is taken back out of the batch and its error
 * returned (ENOSPC for one that did not fit).  A written operation keeps its
 * slot; outside a submission it runs at once and the run's error is
 * returned.
 */
int
drv_i915_gfx_op_end(
	struct i915_render_session *session,
	struct i915_gfx_session *work,
	int error)
{
	int run_error;

	/* An operation that did not fit its room fails as a batch that did not fit. */
	if (error == 0 && work->cursor.overflow != 0)
		error = ENOSPC;

	/* Takes a failed operation back out of the batch. */
	if (error != 0) {
		work->cursor.count = work->op_start;
		work->cursor.overflow = 0;
		return error;
	}

	/* The operation keeps its slot and waits in the batch. */
	work->slot_next++;
	work->ops_pending++;

	/* Outside a submission the operation runs at once. */
	if (!work->open) {
		run_error = drv_i915_gfx_flush(session, work);
		if (run_error != 0)
			return run_error;
	}

	/* Succeeded: the operation is recorded, or has run outside a submission. */
	return 0;
}

/*
 * Runs what the batch holds to its end and empties it.
 *
 * The batch is ended, made visible to the GPU and run on the session's
 * render context; every slot is free again afterwards, whatever the
 * outcome.  Returns 0 (also for an empty batch), or the error of the run.
 */
int
drv_i915_gfx_flush(
	struct i915_render_session *session,
	struct i915_gfx_session *work)
{
	int error;

	/* An empty batch has nothing to run. */
	if (work->ops_pending == 0U) {
		work->cursor.count = 0U;
		work->slot_next = 0U;
		return 0;
	}

	/* Ends the batch and makes the CPU writes visible before the GPU reads them. */
	drv_i915_gfx_emit_batch_end(&work->cursor);
	kern_io_write_barrier();

	/* Runs the batch to its end on the session's render context. */
	error = drv_i915_worker_run_sync(session->vk->i915, &session->gpu->contexts[I915_ENGINE_RCS0], work->batch->va);

	/* Empties the batch: its operations have run, or failed, and every slot is free. */
	work->cursor.count = 0U;
	work->cursor.overflow = 0;
	work->slot_next = 0U;
	work->ops_pending = 0U;
	work->transfer_pending = 0;

	/* Reports a batch that failed on the GPU. */
	if (error != 0) {
		kern_logf("i915: vk: batch failed on the GPU: error %d\n", error);
		return error;
	}

	/* Succeeded: the batch has run to its end. */
	return 0;
}

/*
 * Records one draw or indexed draw into the submission's batch, or runs it
 * to its end outside a submission.
 *
 * The draw needs a prepared pipeline, a render pass and a colour
 * attachment; the depth attachment is optional, and an indexed draw needs
 * an index buffer.  Returns EINVAL or ENOTSUP for a draw the path refuses,
 * ENOMEM when the session's objects cannot be made, ENOSPC when the draw
 * does not fit a batch, or the error of a GPU run.
 */
int
drv_i915_gfx_draw(
	struct i915_render_session *session,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_draw_args *args)
{
	struct i915_gfx_session *work;
	struct i915_gfx_op_space space;
	struct i915_gfx_kernels kernels;
	const struct i915_gfx_image *target;
	const struct i915_gfx_image *depth;
	const char *counted;
	uint64_t target_va;
	uint32_t mocs;
	unsigned dwords;
	int refused;
	int error;

	/* Refuses a draw with no prepared pipeline, render pass or colour attachment bound. */
	refused = 0;
	if (state->pipeline == NULL) {
		refused = 1;
	} else if (state->pipeline->kernels_ready == 0) {
		refused = 1;
	} else if (state->pass == NULL) {
		refused = 1;
	} else if (state->framebuffer == NULL) {
		refused = 1;
	} else if (state->pass->color_attachment >= state->framebuffer->view_count) {
		refused = 1;
	} else if (state->framebuffer->views[state->pass->color_attachment] == NULL) {
		refused = 1;
	}

	/* Says why the draw is refused. */
	if (refused != 0) {
		kern_logf("i915: vk: draw refused: no pipeline, render pass or colour attachment is bound\n");
		return EINVAL;
	}

	/* Takes the colour target, and the depth target when the pass has one. */
	target = state->framebuffer->views[state->pass->color_attachment]->image;
	depth = NULL;
	if (state->pass->depth_attachment < state->framebuffer->view_count &&
	    state->framebuffer->views[state->pass->depth_attachment] != NULL)
		depth = state->framebuffer->views[state->pass->depth_attachment]->image;

	/* Refuses a colour attachment view of another level than 0.  XXX: a draw writes level 0. */
	if (state->framebuffer->views[state->pass->color_attachment]->base_level != 0U) {
		kern_logf("i915: vk: XXX unimplemented path: a draw into mip level %u\n",
			  state->framebuffer->views[state->pass->color_attachment]->base_level);
		return ENOTSUP;
	}

	/* Takes the session's objects, making them on the first draw. */
	work = drv_i915_gfx_session_get(session);
	if (work == NULL)
		return ENOMEM;

	/* Every surface, vertex buffer and state of a draw is uncached. */
	mocs = GEN12_MOCS(I915_MOCS_UNCACHED_INDEX);

	/* Takes the pipeline's kernels and their push data layouts. */
	drv_i915_gfx_pipeline_kernels(state->pipeline, &kernels);

	/*
	 * Uniform data is copied into the push data by the CPU now, so a
	 * rectangle recorded before the draw, which may have written the
	 * buffer, runs first.
	 */
	if (work->transfer_pending != 0) {
		if (kernels.vs_push.block_count != 0U || kernels.ps_push.block_count != 0U) {
			error = drv_i915_gfx_flush(session, work);
			if (error != 0)
				return error;
		}
	}

	/* Gives kernels that spill their part of the scratch buffer. */
	error = i915_draw_scratch(session, work, &kernels);
	if (error != 0)
		return error;

	/* Finds the pipeline's instruction window, placing its kernels the first time. */
	error = drv_i915_gfx_window(session,
				    work,
				    &state->pipeline->kernel_owner,
				    &state->pipeline->kernel_window,
				    &state->pipeline->kernel_generation,
				    kernels.vs_code,
				    kernels.vs_bytes,
				    kernels.ps_code,
				    kernels.ps_bytes,
				    &space);
	if (error != 0)
		return error;

	/* Takes the draw's slot and its room in the batch. */
	error = drv_i915_gfx_op_begin(session, work, &space);
	if (error != 0)
		return error;

	/* Writes the heaps into the slot, then the draw's commands into the batch. */
	error = drv_i915_gfx_write_state(space.slot, state, &kernels, target, mocs);
	if (error == 0) {
		error = i915_draw_build_batch(space.batch,
					      &space,
					      state,
					      &kernels,
					      target,
					      depth,
					      mocs,
					      args);
	}

	/* The draw's size in the batch, for the log of the first draw. */
	dwords = work->cursor.count - work->op_start;

	/* Keeps the draw in the batch, or takes it back; outside a submission it runs now. */
	error = drv_i915_gfx_op_end(session, work, error);
	if (error != 0) {
		kern_logf("i915: vk: draw %u failed: error %d\n", work->draws + 1U, error);
		return error;
	}

	/* An indexed draw counts indices, a draw vertices. */
	counted = "vertices";
	if (args->indexed != 0)
		counted = "indices";

	/* Counts the draw; the first one of the session is logged with its layout. */
	work->draws++;
	if (work->draws == 1U) {
		target_va = drv_i915_gfx_memory_va(target->memory, target->offset);
		kern_logf("i915: vk: first draw: batch %u dwords at 0x%llx, state at 0x%llx, target %ux%u at 0x%llx, "
			  "depth %s, %u %s\n",
			  dwords,
			  (unsigned long long)work->batch->va,
			  (unsigned long long)space.slot_va,
			  target->width,
			  target->height,
			  (unsigned long long)target_va,
			  depth != NULL ? "yes" : "no",
			  args->count,
			  counted);
	}

	/* Lets the test build look at what the first three draws left in the target, which needs them run. */
	if (work->draws <= 3U) {
		if (drv_i915_gfx_draw_checkpoint != NULL) {
			error = drv_i915_gfx_flush(session, work);
			if (error != 0)
				return error;

			drv_i915_gfx_draw_checkpoint(target, work->draws);
		}
	}

	/* Succeeded: the draw is recorded, or has run outside a submission. */
	return 0;
}

/* Makes one session object bound into the session's address space. */
static int
i915_draw_object_create(
	struct i915_render_session *session,
	uint64_t bytes,
	struct i915_gem_object **result)
{
	struct i915_device *device;
	int error;

	/* Creates the object and binds it, destroying it again when the binding fails. */
	device = session->vk->i915;
	mutex_lock(&device->mutex);

	error = drv_i915_gem_create(&device->gem, bytes, result);
	if (error == 0) {
		error = drv_i915_gem_bind_vm(session->gpu->vm, *result);
		if (error != 0)
			drv_i915_gem_destroy(&device->gem, *result);
	}

	mutex_unlock(&device->mutex);

	/* Reports why the object could not be made, with no object. */
	if (error != 0) {
		*result = NULL;
		return error;
	}

	/* Succeeded: the object is bound and ready. */
	return 0;
}

/* Unbinds and destroys one session object. */
static void
i915_draw_object_destroy(
	struct i915_render_session *session,
	struct i915_gem_object *object)
{
	struct i915_device *device;

	/* Unbinds, then destroys, under the device lock. */
	device = session->vk->i915;
	mutex_lock(&device->mutex);

	drv_i915_gem_unbind_vm(object);
	drv_i915_gem_destroy(&device->gem, object);

	mutex_unlock(&device->mutex);
}

/*
 * Gives a draw's kernels that spill their part of the session's scratch
 * buffer, which the draw makes the general state base.
 *
 * Returns 0 with the buffer and the parts' offsets in `kernels`, or the
 * error that kept the buffer from growing.
 */
static int
i915_draw_scratch(
	struct i915_render_session *session,
	struct i915_gfx_session *work,
	struct i915_gfx_kernels *kernels)
{
	int error;
	int roomy;

	/* Kernels that spill nothing need no buffer, and the general state base stays zero. */
	if (kernels->vs_scratch_bytes == 0U && kernels->ps_scratch_bytes == 0U)
		return 0;

	/* Notes whether the buffer has room for both kernels' per-thread spaces. */
	roomy = 0;
	if (work->scratch != NULL &&
	    kernels->vs_scratch_bytes <= work->scratch_per_thread[I915_DRAW_SCRATCH_VERTEX] &&
	    kernels->ps_scratch_bytes <= work->scratch_per_thread[I915_DRAW_SCRATCH_PIXEL])
		roomy = 1;

	/* Grows the buffer when it has not. */
	if (roomy == 0) {
		error = i915_draw_scratch_grow(session, work, kernels);
		if (error != 0)
			return error;
	}

	/* Points the kernels at the buffer and their parts of it. */
	kernels->scratch_base = work->scratch->va;
	kernels->vs_scratch_offset = work->scratch_offset[I915_DRAW_SCRATCH_VERTEX];
	kernels->ps_scratch_offset = work->scratch_offset[I915_DRAW_SCRATCH_PIXEL];

	/* Succeeded: every spilling kernel has its part. */
	return 0;
}

/*
 * Replaces the scratch buffer with one whose parts have room for the
 * kernels' per-thread spaces (and for what the old one had room for) for
 * every thread id of their stages.
 *
 * The vertex stage uses 546 thread ids, the pixel stage 1024 for every
 * slice present (heap.h).  The operations recorded so far may still point
 * at the old buffer, so they run first.  Returns 0, the error of that run,
 * the object's creation error, or ENOTSUP for a buffer larger than the
 * general state.
 */
static int
i915_draw_scratch_grow(
	struct i915_render_session *session,
	struct i915_gfx_session *work,
	const struct i915_gfx_kernels *kernels)
{
	struct i915_device *device;
	struct i915_gem_object *object;
	uint32_t vertex_bytes;
	uint32_t pixel_bytes;
	uint32_t slice_mask;
	uint32_t slices;
	uint64_t pixel_ids;
	uint64_t pixel_offset;
	uint64_t bytes;
	int error;

	/* The recorded operations run before the old buffer goes. */
	if (work->scratch != NULL) {
		error = drv_i915_gfx_flush(session, work);
		if (error != 0)
			return error;

		i915_draw_object_destroy(session, work->scratch);
		work->scratch = NULL;
	}

	/* Each part keeps the room it had and takes what the kernel needs. */
	vertex_bytes = work->scratch_per_thread[I915_DRAW_SCRATCH_VERTEX];
	if (kernels->vs_scratch_bytes > vertex_bytes)
		vertex_bytes = kernels->vs_scratch_bytes;
	pixel_bytes = work->scratch_per_thread[I915_DRAW_SCRATCH_PIXEL];
	if (kernels->ps_scratch_bytes > pixel_bytes)
		pixel_bytes = kernels->ps_scratch_bytes;

	/* Counts the slices the fuses left: the pixel stage's thread ids grow with them. */
	device = session->vk->i915;
	slices = 0U;
	for (slice_mask = device->gt.info.sseu.slice_mask;
	     slice_mask != 0U;
	     slice_mask &= slice_mask - 1U)
		slices++;
	if (slices == 0U)
		slices = 1U;
	pixel_ids = (uint64_t)I915_GFX_PS_SCRATCH_IDS_PER_SLICE * slices;

	/* Lays the buffer out: the guard page, the vertex part, the pixel part, each page-aligned. */
	pixel_offset = I915_GFX_SCRATCH_GUARD + (uint64_t)vertex_bytes * I915_GFX_VS_SCRATCH_IDS;
	pixel_offset = (pixel_offset + I915_GFX_SCRATCH_ALIGN - 1U) & ~(uint64_t)(I915_GFX_SCRATCH_ALIGN - 1U);
	bytes = pixel_offset + (uint64_t)pixel_bytes * pixel_ids;

	/* A buffer beyond the general state's size cannot be addressed. */
	if (bytes > I915_GFX_GENERAL_STATE_BYTES) {
		kern_logf("i915: vk: XXX unimplemented path: a scratch buffer of %llu bytes\n", (unsigned long long)bytes);
		return ENOTSUP;
	}

	/* Makes the buffer. */
	error = i915_draw_object_create(session, bytes, &object);
	if (error != 0) {
		kern_logf("i915: vk: a scratch buffer of %llu bytes cannot be made: error %d\n", (unsigned long long)bytes, error);
		return error;
	}

	/* Keeps the buffer with the session, with the room of each part and where it starts. */
	work->scratch = object;
	work->scratch_per_thread[I915_DRAW_SCRATCH_VERTEX] = vertex_bytes;
	work->scratch_per_thread[I915_DRAW_SCRATCH_PIXEL] = pixel_bytes;
	work->scratch_offset[I915_DRAW_SCRATCH_VERTEX] = I915_GFX_SCRATCH_GUARD;
	work->scratch_offset[I915_DRAW_SCRATCH_PIXEL] = pixel_offset;
	kern_logf("i915: vk: scratch: %llu bytes at 0x%llx: vertex %u bytes a thread for %u ids at +0x%x, pixel %u for %llu ids at +0x%llx\n",
		  (unsigned long long)bytes,
		  (unsigned long long)object->va,
		  vertex_bytes,
		  I915_GFX_VS_SCRATCH_IDS,
		  I915_GFX_SCRATCH_GUARD,
		  pixel_bytes,
		  (unsigned long long)pixel_ids,
		  (unsigned long long)pixel_offset);

	/* Succeeded: the buffer has room for both kernels. */
	return 0;
}

/*
 * Appends the commands of one draw to the batch.
 *
 * The order is the fixture draw's: the context setup at the draw's slot and
 * instruction window, the vertex fetcher (with the index buffer of an
 * indexed draw), the URB and push constants, the state pointers, the
 * geometry stages with only the vertex shader enabled, the rasterizer, the
 * pixel stage, the depth state, and the primitive with its closing flush.
 */
static int
i915_draw_build_batch(
	struct i915_gfx_batch *batch,
	const struct i915_gfx_op_space *space,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_kernels *kernels,
	const struct i915_gfx_image *target,
	const struct i915_gfx_image *depth,
	uint32_t mocs,
	const struct i915_gfx_draw_args *args)
{
	struct i915_gfx_primitive primitive;
	uint64_t state_va;
	uint32_t slots;
	uint32_t entry_size;
	int error;

	/* Switches to 3D, programs the state bases and the once-per-context state. */
	state_va = space->slot_va;
	drv_i915_gfx_emit_context_setup(batch, state_va, space->window_va, kernels->scratch_base, mocs);

	/* Programs the vertex buffers and elements. */
	error = drv_i915_gfx_emit_vertex_input(batch, state, kernels, mocs);
	if (error != 0)
		return error;

	/* Programs the index buffer an indexed draw reads its vertices through. */
	if (args->indexed != 0) {
		error = drv_i915_gfx_emit_index_buffer(batch, state, mocs);
		if (error != 0)
			return error;
	}

	/*
	 * Sizes a VUE entry, in 64-byte units of four 16-byte slots: the vertex
	 * fetcher writes the attributes into the entry the vertex shader then
	 * overwrites with the header, the position and the varyings, so it holds
	 * the larger of the two (brw_compile_vs.cpp, urb_entry_size).
	 */
	slots = 2U + kernels->varyings;
	if (kernels->vs_input_count > slots)
		slots = kernels->vs_input_count;
	entry_size = (slots + 3U) / 4U;
	drv_i915_gfx_emit_urb(batch, entry_size);

	/* Points the vertex and the pixel stage at their push data. */
	drv_i915_gfx_emit_constants(batch,
				    state_va + I915_GFX_PUSH_BUFFER,
				    kernels->vs_push_regs,
				    state_va + I915_GFX_PS_PUSH_BUFFER,
				    kernels->ps_push_regs,
				    mocs);

	/* Points the pipeline at the colour calc, blend, viewport, scissor and coarse pixel state. */
	drv_i915_batch_pointer(batch, GEN12_CMD_3DSTATE_CC_STATE_POINTERS, I915_GFX_DYN_COLOR_CALC | 1U);
	drv_i915_batch_pointer(batch, GEN12_CMD_3DSTATE_BLEND_STATE_POINTERS, I915_GFX_DYN_BLEND | 1U);
	drv_i915_batch_pointer(batch, GEN12_CMD_3DSTATE_VIEWPORT_STATE_POINTERS_CC, I915_GFX_DYN_CC_VIEWPORT);
	drv_i915_batch_pointer(batch, GEN12_CMD_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP, I915_GFX_DYN_SF_CLIP_VIEWPORT);
	drv_i915_batch_pointer(batch, GEN12_CMD_3DSTATE_SCISSOR_STATE_POINTERS, I915_GFX_DYN_SCISSOR);
	drv_i915_batch_pointer(batch, GEN12_CMD_3DSTATE_CPS_POINTERS, I915_GFX_DYN_CPS);

	/* Disables the binding table pool, as anv does against state leaking from other contexts. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_BINDING_TABLE_POOL_ALLOC, GEN12_3DSTATE_BINDING_TABLE_POOL_ALLOC_DWORDS));
	drv_i915_batch_emit(batch, mocs);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);

	/* Renders one sample per pixel. */
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_MULTISAMPLE, GEN12_3DSTATE_MULTISAMPLE_DWORDS);
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_SAMPLE_MASK, GEN12_3DSTATE_SAMPLE_MASK_DWORDS));
	drv_i915_batch_emit(batch, 1U);

	/* Enables the vertex shader and no other geometry stage. */
	drv_i915_gfx_emit_vertex_shader(batch, kernels);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_HS, GEN12_3DSTATE_HS_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_TE, GEN12_3DSTATE_TE_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_DS, GEN12_3DSTATE_DS_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_STREAMOUT, GEN12_3DSTATE_STREAMOUT_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_GS, GEN12_3DSTATE_GS_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_PRIMITIVE_REPLICATION, GEN12_3DSTATE_PRIMITIVE_REPLICATION_DWORDS);

	/* Programs the clipper, setup and rasterizer, then the pixel stage. */
	drv_i915_gfx_emit_raster(batch, state->pipeline);
	drv_i915_gfx_emit_pixel_shader(batch, kernels);

	/* Declares a writeable render target and how it blends. */
	drv_i915_gfx_emit_ps_blend(batch, state->pipeline);

	/* Programs the depth test and buffer. */
	error = drv_i915_gfx_emit_depth(batch, state, depth, state_va + I915_GFX_SCRATCH, mocs);
	if (error != 0)
		return error;

	/*
	 * Describes the triangle list: an indexed draw reads its vertices at
	 * random from the first index on, each index moved by the vertex offset.
	 */
	kern_memset(&primitive, 0, sizeof(primitive));
	primitive.topology = GEN12_3DPRIM_TRILIST;
	primitive.random_access = args->indexed;
	primitive.vertex_count = args->count;
	primitive.start_vertex = args->first;
	primitive.instance_count = args->instance_count;
	primitive.start_instance = args->first_instance;
	primitive.base_vertex = args->vertex_offset;

	/* Draws the triangle list over the target and flushes what it wrote. */
	drv_i915_gfx_emit_draw(batch, target->width, target->height, &primitive);

	/* Succeeded: the draw's commands are in the batch. */
	return 0;
}
