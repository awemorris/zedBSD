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
#define I915_DRAW_SCRATCH_COMPUTE	2U
#define I915_DRAW_SCRATCH_GEOMETRY	3U

static int i915_draw_object_create(struct i915_render_session *session, uint64_t bytes, struct i915_gem_object **result);
static int i915_draw_scratch(struct i915_render_session *session, struct i915_gfx_session *work, struct i915_gfx_kernels *kernels);
static int i915_draw_writes_storage(const struct i915_gfx_kernels *kernels);
static int i915_draw_geometry_check(const struct i915_gfx_pipeline *pipeline, const struct i915_gfx_kernels *kernels);
static int i915_draw_scratch_grow(struct i915_render_session *session, struct i915_gfx_session *work, const struct i915_gfx_kernels *kernels);
static void i915_draw_object_destroy(struct i915_render_session *session, struct i915_gem_object *object);
static int i915_draw_build_batch(struct i915_gfx_batch *batch, const struct i915_gfx_op_space *space, const struct i915_gfx_draw_state *state, const struct i915_gfx_kernels *kernels, const struct i915_gfx_image *target, const struct i915_gfx_image *depth, uint32_t mocs, const struct i915_gfx_draw_args *args);
static void i915_gfx_frame_stat(struct i915_render_session *session, struct i915_gfx_session *work);

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

	/* Counts the submission in the session's frame timing, logged every five seconds. */
	i915_gfx_frame_stat(session, work);

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
 * Finds the instruction window of a set of kernels (vertex, geometry,
 * pixel; NULL for a stage the window does not hold), placing them in a new
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
	const uint32_t *gs_code,
	uint32_t gs_bytes,
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
		if (gs_code != NULL)
			kern_memcpy(address + I915_GFX_GS_KERNEL, gs_code, gs_bytes);
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

	/* A submission's batch that is full runs before its end (the frame timing counts it). */
	if (full && work->open)
		work->stat_full++;

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
	uint64_t start;
	int error;

	/* An empty batch has nothing to run. */
	if (work->ops_pending == 0U) {
		work->cursor.count = 0U;
		work->slot_next = 0U;
		return 0;
	}

	/* Runs the batch to its end on the session's render context, timing it for the frame timing. */
	start = drv_i915_perf_now();
	error = drv_i915_gfx_batch_run(session, &work->cursor, work->batch->va, I915_ENGINE_RCS0);
	work->stat_run_ns += drv_i915_perf_now() - start;
	work->stat_runs++;
	work->stat_ops += work->ops_pending;
	work->stat_ops_now += work->ops_pending;

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
 * Ends a batch and runs it to its end on one of the session's engine contexts.
 */
int
drv_i915_gfx_batch_run(
	struct i915_render_session *session,
	struct i915_gfx_batch *batch,
	uint64_t batch_va,
	unsigned engine)
{
	int error;

	/* Ends the batch and makes the CPU writes visible before the GPU reads them. */
	drv_i915_gfx_emit_batch_end(batch);
	kern_io_write_barrier();

	/* Runs it in the session's context of the engine, to its end. */
	error = drv_i915_worker_run_sync(session->vk->i915, &session->gpu->contexts[engine], batch_va);
	if (error != 0)
		return error;

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
	const struct i915_gfx_view *extent;
	const char *counted;
	uint64_t target_va;
	uint32_t mocs;
	unsigned dwords;
	int writes;
	int refused;
	int error;

	/* Refuses a draw with no prepared pipeline, render pass or attachment bound. */
	refused = 0;
	extent = NULL;
	if (state->pipeline == NULL) {
		refused = 1;
	} else if (state->pipeline->kernels_ready == 0) {
		refused = 1;
	} else if (state->pass == NULL) {
		refused = 1;
	} else if (state->framebuffer == NULL) {
		refused = 1;
	} else {
		extent = drv_i915_gfx_draw_extent_view(state);
		if (extent == NULL)
			refused = 1;
	}

	/* Says why the draw is refused. */
	if (refused != 0) {
		kern_logf("i915: vk: draw refused: no pipeline, render pass or attachment is bound\n");
		return EINVAL;
	}

	/*
	 * Takes the image whose extent the draw covers (the first colour
	 * attachment, or the depth attachment of a pass without colour), and
	 * the depth target when the pass has one.
	 */
	target = extent->image;
	depth = NULL;
	if (state->pass->depth_attachment < state->framebuffer->view_count &&
	    state->framebuffer->views[state->pass->depth_attachment] != NULL)
		depth = state->framebuffer->views[state->pass->depth_attachment]->image;

	/* Takes the session's objects, making them on the first draw. */
	work = drv_i915_gfx_session_get(session);
	if (work == NULL)
		return ENOMEM;

	/* Every surface, vertex buffer and state of a draw is uncached. */
	mocs = GEN12_MOCS(I915_MOCS_UNCACHED_INDEX);

	/* Takes the pipeline's kernels and their push data layouts. */
	drv_i915_gfx_pipeline_kernels(state->pipeline, &kernels);

	/* Refuses a geometry stage the draw cannot run: its input primitive. */
	if (kernels.gs_code != NULL) {
		error = i915_draw_geometry_check(state->pipeline, &kernels);
		if (error != 0)
			return error;
	}

	/*
	 * Uniform data is copied into the push data by the CPU now, so a
	 * rectangle recorded before the draw, which may have written the
	 * buffer, runs first.
	 */
	if (work->transfer_pending != 0) {
		if (kernels.vs_push.block_count != 0U ||
		    kernels.gs_push.block_count != 0U ||
		    kernels.ps_push.block_count != 0U) {
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
				    kernels.gs_code,
				    kernels.gs_bytes,
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

	/*
	 * A draw whose kernels have storage buffers may write them, and a later
	 * operation that copies uniform data on the CPU must wait for it to run
	 * (ws101-p004), as after a rectangle.
	 */
	writes = i915_draw_writes_storage(&kernels);
	if (writes != 0)
		work->transfer_pending = 1;

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

/*
 * Gives the kernels of an operation that spill their parts of the session's
 * scratch buffer, growing it when it has too little room.
 *
 * A dispatch takes its compute kernel's part with it (ws101-p004), as a draw
 * takes its vertex and pixel kernels' parts.  Returns 0 or the error of the
 * buffer's growth.
 */
int
drv_i915_gfx_scratch(
	struct i915_render_session *session,
	struct i915_gfx_session *work,
	struct i915_gfx_kernels *kernels)
{
	int error;

	/* Gives the parts as a draw's kernels get theirs. */
	error = i915_draw_scratch(session, work, kernels);
	if (error != 0)
		return error;

	/* Succeeded: every spilling kernel has its part. */
	return 0;
}

/*
 * Reports whether a draw's kernels name a storage buffer, which they may
 * write: 1 when a stage's push data carries a storage buffer's address.
 */
static int
i915_draw_writes_storage(
	const struct i915_gfx_kernels *kernels)
{
	uint32_t index;

	/* The vertex stage's blocks. */
	for (index = 0U; index < kernels->vs_push.block_count; index++) {
		if (kernels->vs_push.blocks[index].address != 0U)
			return 1;
	}

	/* The geometry stage's blocks. */
	for (index = 0U; index < kernels->gs_push.block_count; index++) {
		if (kernels->gs_push.blocks[index].address != 0U)
			return 1;
	}

	/* The pixel stage's blocks. */
	for (index = 0U; index < kernels->ps_push.block_count; index++) {
		if (kernels->ps_push.blocks[index].address != 0U)
			return 1;
	}

	/* Succeeded: no storage buffer. */
	return 0;
}

/*
 * Checks that a draw can run its pipeline's geometry kernel: the primitives
 * of the draw's topology must have the vertices the kernel expects of its
 * input primitive.  Returns 0, or ENOTSUP with the reason logged.
 */
static int
i915_draw_geometry_check(
	const struct i915_gfx_pipeline *pipeline,
	const struct i915_gfx_kernels *kernels)
{
	uint32_t topology;
	uint32_t vertices;

	/* The vertices of one of the draw's primitives (0 for a topology the vertex input refuses). */
	topology = drv_i915_gfx_topology(pipeline);
	vertices = drv_i915_gfx_topology_vertices(topology);

	/* A primitive of other vertices than the kernel's input primitive is refused. */
	if (vertices != kernels->gs_vertices_in) {
		kern_logf("i915: vk: draw refused: primitive topology %u gives %u vertices a primitive, the geometry shader takes %u\n",
			  pipeline->topology,
			  vertices,
			  kernels->gs_vertices_in);
		return ENOTSUP;
	}

	/* Succeeded: the draw can run the geometry kernel. */
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
	if (kernels->vs_scratch_bytes == 0U &&
	    kernels->gs_scratch_bytes == 0U &&
	    kernels->ps_scratch_bytes == 0U &&
	    kernels->cs_scratch_bytes == 0U)
		return 0;

	/* Notes whether the buffer has room for every kernel's per-thread space. */
	roomy = 0;
	if (work->scratch != NULL &&
	    kernels->vs_scratch_bytes <= work->scratch_per_thread[I915_DRAW_SCRATCH_VERTEX] &&
	    kernels->gs_scratch_bytes <= work->scratch_per_thread[I915_DRAW_SCRATCH_GEOMETRY] &&
	    kernels->ps_scratch_bytes <= work->scratch_per_thread[I915_DRAW_SCRATCH_PIXEL] &&
	    kernels->cs_scratch_bytes <= work->scratch_per_thread[I915_DRAW_SCRATCH_COMPUTE])
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
	kernels->gs_scratch_offset = work->scratch_offset[I915_DRAW_SCRATCH_GEOMETRY];
	kernels->ps_scratch_offset = work->scratch_offset[I915_DRAW_SCRATCH_PIXEL];
	kernels->cs_scratch_offset = work->scratch_offset[I915_DRAW_SCRATCH_COMPUTE];

	/* Succeeded: every spilling kernel has its part. */
	return 0;
}

/*
 * Replaces the scratch buffer with one whose parts have room for the
 * kernels' per-thread spaces (and for what the old one had room for) for
 * every thread id of their stages.
 *
 * The vertex stage uses 546 thread ids, the pixel stage 1024 for every
 * slice present, the compute stage 768 and the geometry stage 336
 * (heap.h).  The operations recorded so far may still point
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
	uint32_t compute_bytes;
	uint32_t geometry_bytes;
	uint32_t slice_mask;
	uint32_t slices;
	uint64_t pixel_ids;
	uint64_t pixel_offset;
	uint64_t compute_offset;
	uint64_t geometry_offset;
	uint64_t bytes;
	int error;

	/* The recorded operations run before the old buffer goes. */
	if (work->scratch != NULL) {
		error = drv_i915_gfx_flush(session, work);
		if (error != 0)
			return error;

		/* Then the old buffer goes, and the session holds none until the new one is made. */
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
	compute_bytes = work->scratch_per_thread[I915_DRAW_SCRATCH_COMPUTE];
	if (kernels->cs_scratch_bytes > compute_bytes)
		compute_bytes = kernels->cs_scratch_bytes;
	geometry_bytes = work->scratch_per_thread[I915_DRAW_SCRATCH_GEOMETRY];
	if (kernels->gs_scratch_bytes > geometry_bytes)
		geometry_bytes = kernels->gs_scratch_bytes;

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

	/* Lays the buffer out: the guard page, the vertex, pixel, compute and geometry parts, each page-aligned. */
	pixel_offset = I915_GFX_SCRATCH_GUARD + (uint64_t)vertex_bytes * I915_GFX_VS_SCRATCH_IDS;
	pixel_offset = (pixel_offset + I915_GFX_SCRATCH_ALIGN - 1U) & ~(uint64_t)(I915_GFX_SCRATCH_ALIGN - 1U);
	compute_offset = pixel_offset + (uint64_t)pixel_bytes * pixel_ids;
	compute_offset = (compute_offset + I915_GFX_SCRATCH_ALIGN - 1U) & ~(uint64_t)(I915_GFX_SCRATCH_ALIGN - 1U);
	geometry_offset = compute_offset + (uint64_t)compute_bytes * I915_GFX_CS_SCRATCH_IDS;
	geometry_offset = (geometry_offset + I915_GFX_SCRATCH_ALIGN - 1U) & ~(uint64_t)(I915_GFX_SCRATCH_ALIGN - 1U);
	bytes = geometry_offset + (uint64_t)geometry_bytes * I915_GFX_GS_SCRATCH_IDS;

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
	work->scratch_per_thread[I915_DRAW_SCRATCH_COMPUTE] = compute_bytes;
	work->scratch_offset[I915_DRAW_SCRATCH_COMPUTE] = compute_offset;
	work->scratch_per_thread[I915_DRAW_SCRATCH_GEOMETRY] = geometry_bytes;
	work->scratch_offset[I915_DRAW_SCRATCH_GEOMETRY] = geometry_offset;
	kern_logf("i915: vk: scratch: %llu bytes at 0x%llx: vertex %u bytes a thread for %u ids at +0x%x, pixel %u for %llu ids at +0x%llx, "
		  "compute %u for %u ids at +0x%llx, geometry %u for %u ids at +0x%llx\n",
		  (unsigned long long)bytes,
		  (unsigned long long)object->va,
		  vertex_bytes,
		  I915_GFX_VS_SCRATCH_IDS,
		  I915_GFX_SCRATCH_GUARD,
		  pixel_bytes,
		  (unsigned long long)pixel_ids,
		  (unsigned long long)pixel_offset,
		  compute_bytes,
		  I915_GFX_CS_SCRATCH_IDS,
		  (unsigned long long)compute_offset,
		  geometry_bytes,
		  I915_GFX_GS_SCRATCH_IDS,
		  (unsigned long long)geometry_offset);

	/* Succeeded: the buffer has room for every kernel. */
	return 0;
}

/*
 * Appends the commands of one draw to the batch.
 *
 * The order is the fixture draw's: the context setup at the draw's slot and
 * instruction window, the vertex fetcher (with the index buffer of an
 * indexed draw), the URB and push constants, the state pointers, the
 * vertex and the geometry stage (no tessellation), the rasterizer, the
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
	uint32_t level;
	uint32_t width;
	uint32_t height;
	uint32_t samples;
	uint32_t sample_mask;
	uint32_t streamout;
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
	 * the larger of the two (brw_compile_vs.cpp, urb_entry_size).  With a
	 * geometry kernel the varyings are the vertex kernel's own, not the
	 * geometry kernel's the setup reads.
	 */
	slots = 2U + kernels->varyings;
	if (kernels->gs_code != NULL)
		slots = 2U + kernels->vs_varyings;
	if (kernels->vs_input_count > slots)
		slots = kernels->vs_input_count;
	entry_size = (slots + 3U) / 4U;

	/* Allocates the push constants and the URB, the geometry stage's entries of its kernel's size. */
	error = drv_i915_gfx_emit_urb(batch, entry_size, kernels->gs_urb_entry_size);
	if (error != 0) {
		kern_logf("i915: vk: draw refused: the geometry stage's URB entries (%u x 64 bytes) leave room for fewer than two\n",
			  kernels->gs_urb_entry_size);
		return error;
	}

	/* Points the vertex, the geometry and the pixel stage at their push data. */
	drv_i915_gfx_emit_constants(batch,
				    state_va + I915_GFX_PUSH_BUFFER,
				    kernels->vs_push_regs,
				    state_va + I915_GFX_GS_PUSH_BUFFER,
				    kernels->gs_push_regs,
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

	/*
	 * Takes the pipeline's samples per pixel and the ones it writes; a
	 * pipeline no create decoded (the executor's own tests build theirs)
	 * has no samples recorded and writes its one sample.
	 */
	samples = state->pipeline->samples;
	sample_mask = state->pipeline->sample_mask;
	if (samples == 0U) {
		samples = 1U;
		sample_mask = 1U;
	}

	/* Renders the samples at the pixel centre, writing the ones the mask names. */
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_MULTISAMPLE, GEN12_3DSTATE_MULTISAMPLE_DWORDS));
	drv_i915_batch_emit(batch, drv_i915_gfx_samples_log2(samples) << GEN12_MULTISAMPLE_COUNT_SHIFT);
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_SAMPLE_MASK, GEN12_3DSTATE_SAMPLE_MASK_DWORDS));
	drv_i915_batch_emit(batch, sample_mask & ((1U << samples) - 1U));

	/* Enables the vertex shader and neither tessellation stage. */
	drv_i915_gfx_emit_vertex_shader(batch, kernels);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_HS, GEN12_3DSTATE_HS_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_TE, GEN12_3DSTATE_TE_DWORDS);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_DS, GEN12_3DSTATE_DS_DWORDS);

	/*
	 * No stream output; a discarding pipeline's primitives stop there, so
	 * its draws run the vertex shader (and its stores) and rasterize nothing.
	 */
	streamout = 0U;
	if (state->pipeline->rasterizer_discard != 0U)
		streamout = GEN12_STREAMOUT_RENDERING_DISABLE;
	drv_i915_batch_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_STREAMOUT, GEN12_3DSTATE_STREAMOUT_DWORDS));
	drv_i915_batch_emit(batch, streamout);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);

	/* The geometry kernel when the pipeline has one, and no primitive replication. */
	drv_i915_gfx_emit_geometry_shader(batch, kernels);
	drv_i915_batch_zero(batch, GEN12_CMD_3DSTATE_PRIMITIVE_REPLICATION, GEN12_3DSTATE_PRIMITIVE_REPLICATION_DWORDS);

	/* Programs the clipper, setup and rasterizer, then the pixel stage. */
	drv_i915_gfx_emit_raster(batch, state->pipeline, kernels);
	drv_i915_gfx_emit_pixel_shader(batch, kernels);

	/* Declares a writeable render target and how it blends. */
	drv_i915_gfx_emit_ps_blend(batch, state->pipeline);

	/* Programs the depth test and buffer. */
	error = drv_i915_gfx_emit_depth(batch, state, depth, state_va + I915_GFX_SCRATCH, mocs);
	if (error != 0)
		return error;

	/*
	 * Describes the primitives of the pipeline's topology (the vertex input
	 * refused any other): an indexed draw reads its vertices at random from
	 * the first index on, each index moved by the vertex offset.
	 */
	kern_memset(&primitive, 0, sizeof(primitive));
	primitive.topology = drv_i915_gfx_topology(state->pipeline);
	primitive.random_access = args->indexed;
	primitive.vertex_count = args->count;
	primitive.start_vertex = args->first;
	primitive.instance_count = args->instance_count;
	primitive.start_instance = args->first_instance;
	primitive.base_vertex = args->vertex_offset;

	/* Draws the triangle list over the level of the target its view writes and flushes what it wrote. */
	level = drv_i915_gfx_draw_extent_view(state)->base_level;
	width = target->width >> level;
	if (width == 0U)
		width = 1U;
	height = target->height >> level;
	if (height == 0U)
		height = 1U;
	drv_i915_gfx_emit_draw(batch, width, height, &primitive);

	/* Succeeded: the draw's commands are in the batch. */
	return 0;
}

/*
 * Finds the view whose level a draw covers: the first colour slot that has
 * a view, or the depth attachment's view for a pass that draws no colour;
 * NULL when the framebuffer has neither.
 */
const struct i915_gfx_view *
drv_i915_gfx_draw_extent_view(
	const struct i915_gfx_draw_state *state)
{
	uint32_t attachment;
	uint32_t slot;

	/* The first colour slot with a view. */
	for (slot = 0U; slot < I915_GFX_MAX_COLOR_ATTACHMENTS; slot++) {
		attachment = drv_i915_gfx_pass_color(state->pass, slot);
		if (attachment < state->framebuffer->view_count && state->framebuffer->views[attachment] != NULL)
			return state->framebuffer->views[attachment];
	}

	/* Else the depth attachment. */
	attachment = state->pass->depth_attachment;
	if (attachment < state->framebuffer->view_count)
		return state->framebuffer->views[attachment];

	/* Succeeded: the pass has no attachment to draw. */
	return NULL;
}

/* Makes a GPU object of `bytes` bound into the session's address space (a query pool's counters). */
int
drv_i915_gfx_object_create(
	struct i915_render_session *session,
	uint64_t bytes,
	struct i915_gem_object **result)
{
	int error;

	/* As the session's own objects are made. */
	error = i915_draw_object_create(session, bytes, result);
	return error;
}

/* Unbinds and destroys an object drv_i915_gfx_object_create() made. */
void
drv_i915_gfx_object_destroy(
	struct i915_render_session *session,
	struct i915_gem_object *object)
{
	/* As the session's own objects are destroyed. */
	i915_draw_object_destroy(session, object);
}

/*
 * Counts one ended submission in the session's frame timing and logs the
 * window once five seconds have passed (ws075-p026): per submission, the
 * runs and operations, the time inside the submission and in its runs
 * (the rest of it is the executor recording the operations on the CPU),
 * and the time between submissions (the application's own work and
 * waits).  Only a session that submits at least ten times a second is
 * logged.
 */
static void
i915_gfx_frame_stat(
	struct i915_render_session *session,
	struct i915_gfx_session *work)
{
	uint64_t now;
	uint64_t window;
	uint64_t per;

	/* The submission's time, from its start to now. */
	now = drv_i915_perf_now();
	if (work->stat_start == 0U)
		work->stat_start = work->submit_start;
	work->stat_submits++;
	work->stat_submit_ns += now - work->submit_start;
	if (work->stat_ops_now > work->stat_ops_max)
		work->stat_ops_max = work->stat_ops_now;
	work->stat_ops_now = 0U;

	/* A window shorter than five seconds is not logged yet. */
	window = now - work->stat_start;
	if (window < 5000000000ULL)
		return;

	/* The busy sessions' line: per submission, in microseconds. */
	if (work->stat_submits >= 50U) {
		per = work->stat_submits;
		kern_logf("i915: vk: session %u: frames %u in %u ms: per submit %u runs (%u full) %u ops (most %u), in submit %u us (runs %u, record %u), between %u us\n",
			  session->gpu->identifier,
			  work->stat_submits,
			  (unsigned)(window / 1000000U),
			  (unsigned)((work->stat_runs + per / 2U) / per),
			  work->stat_full,
			  (unsigned)(work->stat_ops / per),
			  work->stat_ops_max,
			  (unsigned)(work->stat_submit_ns / per / 1000U),
			  (unsigned)(work->stat_run_ns / per / 1000U),
			  (unsigned)((work->stat_submit_ns - work->stat_run_ns) / per / 1000U),
			  (unsigned)((window - work->stat_submit_ns) / per / 1000U));
	}

	/* A new window starts. */
	work->stat_start = now;
	work->stat_submits = 0U;
	work->stat_runs = 0U;
	work->stat_ops = 0U;
	work->stat_full = 0U;
	work->stat_ops_max = 0U;
	work->stat_submit_ns = 0U;
	work->stat_run_ns = 0U;
}
