/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The render node's submissions: the node's stream, the binding of a
 * session's tables to the node's address space slot, the call of a
 * command stream through the ring and the wait for its end, the
 * protocol's capset, command frames and supervised jobs, and recovery.
 *
 * One submission runs at a time, under the device's lock, and the
 * submission returns only once its work ended (or was stopped).  So a
 * command frame's callback and a job marker's completion are delivered
 * at once: every work the session sent before them has ended.  A
 * submission that faults or runs out of time has its group terminated
 * and started again, and its session is reported lost.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>
#include <uapi/gpu-job.h>

#include "drivers/gpu/mali/cs/cs-build.h"
#include "drivers/gpu/mali/render.h"

/* The node's stream: group slot 1, stream 0, doorbell 2. */
#define RENDER_GROUP_SLOT	1U
#define RENDER_STREAM		0U
#define RENDER_DOORBELL		2U

/* The most instructions the ring takes for one submission. */
#define RING_WORDS		16U

/* The scoreboard entry the ring's flush signals, and the mask of every entry. */
#define RING_FLUSH_SLOT		0U
#define WAIT_ALL		0xffffU

/*
 * The protocol's capset: its id, its bytes and the places of its words,
 * the wire version, the XML version (1.3.269), the timelines word, and
 * the vendor suffix ("SDBZ") with its flags (opaque sharing, strict
 * queues and checked quiescence).
 *
 * Values referenced: zedBSD userland/desktop/libvulkan/context.c (the
 * capset the transport asks for and the words it reads) and
 * userland/desktop/libvulkan/device.c (the profile a device needs).
 */
#define CAPSET_ID		4U
#define CAPSET_BYTES		168U
#define CAPSET_WIRE_AT		0U
#define CAPSET_XML_AT		4U
#define CAPSET_TIMELINES_AT	152U
#define CAPSET_MAGIC_AT		160U
#define CAPSET_FLAGS_AT		164U
#define CAPSET_WIRE_VERSION	1U
#define CAPSET_XML_VERSION	((1U << 22) | (3U << 12) | 269U)
#define CAPSET_VENDOR_MAGIC	0x5a424453U
#define CAPSET_VENDOR_FLAGS	7U

/* The timelines a frame or a job may name (0 is the context's own). */
#define RUNTIME_TIMELINES	64U

static int sched_capset(void *device, void *session, struct gpu_capset *capset);
static int sched_command(void *device, void *session, const void *wire, uint32_t bytes);
static int sched_submit(void *device, void *session, const void *command, uint32_t bytes, uint32_t flags, uint32_t timeline, struct drv_gpu_completion *completion);
static void sched_drain(void *device, void *session);
static int sched_reserve(void *device, void *session, uint32_t timeline, struct drv_gpu_completion *completion, void **token);
static int sched_commit(void *device, void *session, void *token, struct drv_gpu_completion *completion);
static int sched_cancel(void *device, void *session, void *token, struct drv_gpu_completion *completion, unsigned fault);
static int sched_capacity(void *device, void *session, uint32_t timeline, unsigned *available);
static int sched_stop_begin(void *device, void *session, int error);
static int sched_stop_poll(void *device, void *session);
static void sched_fault(void *device, int error);
static int sched_reset(void *device);
static int sched_isolate(void *device, void *session);
static int bind_session(struct mali_render *render, struct mali_session *session);
static int run_frame(struct mali_session *session, const void *wire, uint32_t bytes);
static int marker_index(const struct mali_session *session, const void *token, unsigned *index);
static bool refused(struct mali_render *render, struct mali_session *session);
static void put32(uint8_t *bytes, uint32_t at, uint32_t value);

/* The node's queue: frames run at once, their callbacks completed in order. */
static const struct drv_gpu_command_ops command_operations = {
	.submit = sched_submit,
	.drain = sched_drain,
};

/* The node's supervised jobs: markers after the session's work, which has ended when they commit. */
static const struct drv_gpu_job_ops job_operations = {
	.reserve = sched_reserve,
	.commit = sched_commit,
	.cancel = sched_cancel,
	.capacity = sched_capacity,
};

/* The node's recovery: a session stopped or isolated, or the node faulted and reset. */
static const struct drv_gpu_recovery_ops recovery_operations = {
	.stop_begin = sched_stop_begin,
	.stop_poll = sched_stop_poll,
	.fault = sched_fault,
	.reset = sched_reset,
	.isolate = sched_isolate,
};

/*
 * Starts the node's stream: group slot 1, stream 0, in the node's slot
 * with its idle tables, on the node's ring.
 */
int
mali_sched_start(
	struct mali_render *render)
{
	struct mali_queue *queue;
	int error;

	/* The stream's slot, its doorbell and its space; the ring at its fixed place. */
	queue = &render->queue;
	queue->group_slot = RENDER_GROUP_SLOT;
	queue->stream = RENDER_STREAM;
	queue->doorbell = RENDER_DOORBELL;
	queue->space = &render->idle;
	queue->ring = render->ring->buffer;
	queue->ring.gpu_address = MALI_RENDER_RING_ADDRESS;

	/* Programs and starts it. */
	error = mali_queue_open(render->device, queue);
	if (error != 0) {
		kern_logf("mali: render: stream not started (%d)\n", error);
		return error;
	}

	/* Succeeded: the stream waits for work. */
	return 0;
}

/*
 * Runs a command stream of a session: binds the session's tables to the
 * node's slot, calls the stream from the ring, cleans the caches, adds
 * one to the sync object and waits for it.  A fault or a timeout stops
 * and restarts the node's group, writes the stream's state, and loses
 * the session.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_sched.c (queue_run_job:
 * the flush, the call, the wait for every scoreboard entry, the 64-bit
 * sync add of system scope that carries an error, the error barrier).
 */
int
mali_sched_run(
	struct mali_session *session,
	uint64_t stream_address,
	uint32_t stream_bytes)
{
	struct mali_queue_fault fault;
	struct mali_render *render;
	struct mali_device *device;
	struct mali_queue *queue;
	struct mali_cs cs;
	uint64_t words[RING_WORDS];
	volatile uint64_t *sync;
	unsigned address;
	unsigned value;
	uint64_t target;
	unsigned long enabled;
	bool waited;
	bool failed;
	bool preloaded;
	uint32_t meta;
	int error;

	/* Whether the stream runs a pre-frame draw (said while it was built); the next stream starts without. */
	preloaded = session->preload_used;
	session->preload_used = false;

	/* The executor's own programs the stream runs (said while it was built); the next stream starts without. */
	meta = session->meta_used;
	session->meta_used = 0;

	/* Nothing to call. */
	kern_memset(&fault, 0, sizeof(fault));
	waited = false;
	failed = false;
	render = session->render;
	device = render->device;
	queue = &render->queue;
	if (stream_bytes == 0U)
		return 0;

	/*
	 * The ring's sequence: flush, call, wait, clean, add, barrier.  Each
	 * flush names flush id zero, so it is never skipped as done already.
	 */
	address = mali_queue_scratch(queue);
	value = address + 2U;
	mali_cs_init(&cs, words, RING_WORDS);
	mali_cs_emit(&cs, mali_cs_move32(value, 0));
	mali_cs_emit(&cs, mali_cs_flush_cache(MALI_CS_FLUSH_CLEAN_INVALIDATE, MALI_CS_FLUSH_CLEAN_INVALIDATE, MALI_CS_FLUSH_OTHER_INVALIDATE, 0, value, RING_FLUSH_SLOT));
	mali_cs_emit(&cs, mali_cs_move48(address, stream_address));
	mali_cs_emit(&cs, mali_cs_move32(value, stream_bytes));
	mali_cs_emit(&cs, mali_cs_wait(1U << RING_FLUSH_SLOT));
	mali_cs_emit(&cs, mali_cs_call(address, value));
	mali_cs_emit(&cs, mali_cs_wait(WAIT_ALL));
	mali_cs_emit(&cs, mali_cs_move32(value, 0));
	mali_cs_emit(&cs, mali_cs_flush_cache(MALI_CS_FLUSH_CLEAN_INVALIDATE, MALI_CS_FLUSH_CLEAN_INVALIDATE, MALI_CS_FLUSH_OTHER_INVALIDATE, 0, value, RING_FLUSH_SLOT));
	mali_cs_emit(&cs, mali_cs_wait(1U << RING_FLUSH_SLOT));
	mali_cs_emit(&cs, mali_cs_move48(address, MALI_RENDER_SYNC_ADDRESS));
	mali_cs_emit(&cs, mali_cs_move48(value, 1U));
	mali_cs_emit(&cs, mali_cs_sync_add64(true, MALI_CS_SCOPE_SYSTEM, 0, value, address, 0));
	mali_cs_emit(&cs, mali_cs_error_barrier());
	if (cs.error != 0)
		return E2BIG;

	/* One submission at a time on the GPU. */
	mutex_lock(&device->lock);

	/* A faulted node or a lost session runs nothing. */
	error = 0;
	if (refused(render, session))
		error = EIO;

	/* The session's tables in the slot. */
	if (error == 0)
		error = bind_session(render, session);

	/* Into the ring; the sync object is waited for to reach one more. */
	if (error == 0) {
		target = render->submitted + 1U;
		error = mali_queue_push(device, queue, words, cs.count);
		if (error == 0)
			render->submitted = target;
	}

	if (error == 0) {
		sync = (volatile uint64_t *)(volatile void *)render->sync->buffer.view;
		error = mali_queue_wait(device, queue, sync, target, MALI_RENDER_SUBMIT_MS, &fault);
		waited = true;
		render->submissions++;
	}

	/* A fault or a timeout: the state, the group stopped and started again, the session lost. */
	if (waited && (error == EIO || error == ETIMEDOUT)) {
		failed = true;
		render->failures++;
		kern_logf("mali: render: submission %u failed (%d): stream %llx bytes %u, fault %08x (%llx), fatal %08x (%llx), %u faults\n",
			  render->submissions,
			  error,
			  (unsigned long long)stream_address,
			  (unsigned)stream_bytes,
			  (unsigned)fault.fault,
			  (unsigned long long)fault.fault_info,
			  (unsigned)fault.fatal,
			  (unsigned long long)fault.fatal_info,
			  fault.faults);
		mali_queue_report(device, queue, "render");
		mali_gpu_report_events(device);
		session->lost = true;
		if (mali_sched_restart(render) != 0)
			render->faulted = true;
		sync = (volatile uint64_t *)(volatile void *)render->sync->buffer.view;
		*sync = render->submitted;
	} else if (error == 0 && fault.faults != 0U) {
		kern_logf("mali: render: submission %u ended after %u faults (last %08x, %llx)\n",
			  render->submissions,
			  fault.faults,
			  (unsigned)fault.fault,
			  (unsigned long long)fault.fault_info);
	}

	mutex_unlock(&device->lock);

	/*
	 * A failed stream that ran a pre-frame draw stops every later pass
	 * of the node from using one: their loads draw on cleared tiles, as
	 * without the draw, rather than risk the fault again.
	 */
	if (failed && preloaded) {
		enabled = spin_lock_irqsave(&render->guard);

		render->preload_refused = true;

		spin_unlock_irqrestore(&render->guard, enabled);

		kern_logf("mali: render: submission %u ran a pre-frame load; later loads draw on cleared tiles\n", render->submissions);
	}

	/*
	 * A failed submission that ran the executor's own programs (a scaled
	 * blit, a clear inside a pass, a copy or a clear of one aspect of a
	 * depth and stencil image) stops the node from using them: that
	 * work is left out again, as without them, rather than risk the fault
	 * again.
	 */
	if (failed && meta != 0U) {
		enabled = spin_lock_irqsave(&render->guard);

		render->meta_refused |= meta;

		spin_unlock_irqrestore(&render->guard, enabled);

		kern_logf("mali: render: submission %u ran the executor's own programs (%x); later work that needs them (blits, clears, copies of one aspect) is not run\n", render->submissions, (unsigned)meta);
	}

	/* A lost session's core is told; it stops the session. */
	if (session->lost && render->node != NULL)
		drv_gpu_report_session_error(render->node, session, EIO);

	/* Propagates the failure. */
	if (error != 0)
		return error;

	/* Succeeded: the work has ended. */
	return 0;
}

/*
 * Makes the node's slot forget a closing session's tables: the idle ones
 * take their place.
 */
void
mali_sched_forget(
	struct mali_render *render,
	struct mali_session *session)
{
	/* Only the bound session's tables are in the slot. */
	mutex_lock(&render->device->lock);

	if (render->bound == session) {
		mali_as_disable(render->device, &session->space);
		(void)mali_as_enable(render->device, &render->idle);
		render->bound = NULL;
	}

	mutex_unlock(&render->device->lock);
}

/*
 * Stops the node's group and starts it again on its idle tables, after a
 * submission faulted or ran out of time.  The caller holds the device's
 * lock.
 */
int
mali_sched_restart(
	struct mali_render *render)
{
	struct mali_device *device;
	int error;

	/* The group stops; the GPU no longer reads any session's memory. */
	device = render->device;
	error = mali_queue_stop(device, &render->queue);
	if (error != 0)
		return error;

	/* The idle tables in the slot. */
	if (render->bound != NULL) {
		mali_as_disable(device, &render->bound->space);
		render->bound = NULL;
	}

	error = mali_as_enable(device, &render->idle);
	if (error != 0)
		return error;

	/* The group again, on an empty ring. */
	render->queue.space = &render->idle;
	error = mali_queue_open(device, &render->queue);
	if (error != 0)
		return error;

	/* Succeeded: the node takes work again. */
	return 0;
}

/* Binds the capset, the frames, the jobs and the recovery to the node's table. */
void
mali_sched_bind_ops(
	struct mali_render *render)
{
	render->ops.get_capset = sched_capset;
	render->ops.command = sched_command;
	render->ops.commands = &command_operations;
	render->ops.jobs = &job_operations;
	render->ops.recovery = &recovery_operations;
	render->ops.capabilities |= GPU_CAP_CAPSET | GPU_CAP_COMMAND | GPU_CAP_JOB | GPU_CAP_JOB_CAPACITY;
}

/* Reports the protocol's capset: wire 1, XML 1.3.269, timelines, the vendor suffix and its flags. */
static int
sched_capset(
	void *device,
	void *session,
	struct gpu_capset *capset)
{
	/* Only the protocol's capset, version 0, in a reply that holds it. */
	(void)device;
	(void)session;
	if (capset->capset_id != CAPSET_ID || capset->capset_version != 0U)
		return ENOTSUP;
	if (capset->capacity < CAPSET_BYTES || capset->capacity > GPU_CAPSET_MAX)
		return EINVAL;

	/* Every word not named is zero. */
	kern_memset(capset->data, 0, sizeof(capset->data));
	put32(capset->data, CAPSET_WIRE_AT, CAPSET_WIRE_VERSION);
	put32(capset->data, CAPSET_XML_AT, CAPSET_XML_VERSION);
	put32(capset->data, CAPSET_TIMELINES_AT, 1U);
	put32(capset->data, CAPSET_MAGIC_AT, CAPSET_VENDOR_MAGIC);
	put32(capset->data, CAPSET_FLAGS_AT, CAPSET_VENDOR_FLAGS);
	capset->bytes = CAPSET_BYTES;

	/* Succeeded: the capset is reported. */
	return 0;
}

/* Runs one command frame at once. */
static int
sched_command(
	void *device,
	void *opaque,
	const void *wire,
	uint32_t bytes)
{
	struct mali_session *session;
	int error;

	/* Whole words within the bound, of a session that takes work. */
	session = opaque;
	if (wire == NULL || bytes == 0U || bytes > GPU_COMMAND_MAX || (bytes & 3U) != 0U)
		return EINVAL;
	if (refused(device, session))
		return EIO;

	/* The frame. */
	error = run_frame(session, wire, bytes);
	if (error != 0)
		return error;

	/* Succeeded: the frame ran. */
	return 0;
}

/*
 * Takes one command frame (or an empty fence frame) with its callback:
 * the frame runs at once, and the callback completes with its result,
 * since every work the session sent before it has ended.
 */
static int
sched_submit(
	void *device,
	void *opaque,
	const void *command,
	uint32_t bytes,
	uint32_t flags,
	uint32_t timeline,
	struct drv_gpu_completion *completion)
{
	struct mali_session *session;
	int error;

	/* Only the context's fence flag; a timeline only with it. */
	session = opaque;
	if ((flags & ~GPU_COMMAND_CONTEXT_FENCE) != 0U || timeline >= RUNTIME_TIMELINES)
		return EINVAL;
	if (flags == 0U && timeline != 0U)
		return EINVAL;

	/* Whole words within the bound; an empty frame only as a fence. */
	if (bytes > GPU_COMMAND_MAX || (bytes & 3U) != 0U)
		return EINVAL;
	if (bytes == 0U && flags == 0U)
		return EINVAL;
	if (bytes != 0U && command == NULL)
		return EINVAL;

	/* A lost session takes nothing. */
	if (refused(device, session))
		return EIO;

	/* The frame, whose result the callback carries. */
	error = 0;
	if (bytes != 0U)
		error = run_frame(session, command, bytes);
	drv_gpu_complete(completion, error);

	/* Succeeded: the callback completed. */
	return 0;
}

/* Ends every accepted callback: they all completed when they were accepted. */
static void
sched_drain(
	void *device,
	void *session)
{
	(void)device;
	(void)session;
}

/* Reserves a job marker on a timeline of the session's queue. */
static int
sched_reserve(
	void *device,
	void *opaque,
	uint32_t timeline,
	struct drv_gpu_completion *completion,
	void **token)
{
	struct mali_render *render;
	struct mali_session *session;
	unsigned long enabled;
	unsigned index;
	bool owned;
	int error;

	/* A timeline of the session's queue. */
	render = device;
	session = opaque;
	*token = NULL;
	owned = false;
	if (timeline != 0U && timeline < RUNTIME_TIMELINES && session->vk != NULL)
		owned = mali_vk_timeline_owned(session->vk, timeline);
	if (!owned)
		return EINVAL;

	/* A free marker of a session that takes work. */
	enabled = spin_lock_irqsave(&render->guard);

	error = EAGAIN;
	if (render->faulted || session->lost || session->stopping || session->isolated) {
		error = EIO;
	} else {
		for (index = 0; index < MALI_RENDER_MARKERS; index++) {
			if (session->markers[index].state == MALI_MARKER_FREE) {
				session->markers[index].state = MALI_MARKER_RESERVED;
				session->markers[index].completion = completion;
				session->reserved++;
				*token = &session->markers[index];
				error = 0;
				break;
			}
		}
	}

	spin_unlock_irqrestore(&render->guard, enabled);

	/* No marker. */
	if (error != 0)
		return error;

	/* Succeeded: the token names the marker. */
	return 0;
}

/* Commits a reserved marker: the session's earlier work has ended, so it completes at once. */
static int
sched_commit(
	void *device,
	void *opaque,
	void *token,
	struct drv_gpu_completion *completion)
{
	struct mali_render *render;
	struct mali_session *session;
	unsigned long enabled;
	unsigned index;
	int error;

	/* The marker the token names. */
	render = device;
	session = opaque;
	error = marker_index(session, token, &index);
	if (error != 0)
		return error;

	/* A reservation of this completion, freed for the next. */
	enabled = spin_lock_irqsave(&render->guard);

	error = 0;
	if (session->markers[index].state != MALI_MARKER_RESERVED || session->markers[index].completion != completion) {
		error = ESTALE;
	} else if (render->faulted) {
		error = EIO;
	} else {
		session->markers[index].state = MALI_MARKER_FREE;
		session->markers[index].completion = NULL;
		session->reserved--;
	}

	spin_unlock_irqrestore(&render->guard, enabled);

	if (error != 0)
		return error;

	/* The completion, with the session's loss when its work failed. */
	if (session->lost) {
		drv_gpu_complete(completion, EIO);
	} else {
		drv_gpu_complete(completion, 0);
	}

	/* Succeeded: the marker completed. */
	return 0;
}

/*
 * Withdraws a reservation (normal: no completion) or ends it with EIO
 * (fault).  A committed marker has completed and is free again.
 */
static int
sched_cancel(
	void *device,
	void *opaque,
	void *token,
	struct drv_gpu_completion *completion,
	unsigned fault)
{
	struct mali_render *render;
	struct mali_session *session;
	unsigned long enabled;
	unsigned index;
	int error;

	/* Normal or fault only, of a marker the token names. */
	render = device;
	session = opaque;
	if (fault > 1U)
		return EINVAL;
	error = marker_index(session, token, &index);
	if (error != 0)
		return error;

	/* Only a reservation of this completion is withdrawn. */
	enabled = spin_lock_irqsave(&render->guard);

	error = 0;
	if (session->markers[index].state != MALI_MARKER_RESERVED || session->markers[index].completion != completion) {
		error = ESTALE;
	} else {
		session->markers[index].state = MALI_MARKER_FREE;
		session->markers[index].completion = NULL;
		session->reserved--;
	}

	spin_unlock_irqrestore(&render->guard, enabled);

	if (error != 0)
		return error;

	/* A faulted reservation ends with the error. */
	if (fault != 0U)
		drv_gpu_complete(completion, EIO);

	/* Succeeded: the reservation is withdrawn or ended. */
	return 0;
}

/* Reports how many markers a reservation on the timeline could take now. */
static int
sched_capacity(
	void *device,
	void *opaque,
	uint32_t timeline,
	unsigned *available)
{
	struct mali_render *render;
	struct mali_session *session;
	unsigned long enabled;
	bool owned;
	int error;

	/* A timeline of the session's queue. */
	render = device;
	session = opaque;
	*available = 0;
	owned = false;
	if (timeline != 0U && timeline < RUNTIME_TIMELINES && session->vk != NULL)
		owned = mali_vk_timeline_owned(session->vk, timeline);
	if (!owned)
		return EINVAL;

	/* The free markers, unless the node faulted. */
	enabled = spin_lock_irqsave(&render->guard);

	error = 0;
	if (render->faulted)
		error = EIO;
	else
		*available = MALI_RENDER_MARKERS - session->reserved;

	spin_unlock_irqrestore(&render->guard, enabled);

	if (error != 0)
		return error;

	/* Succeeded: the count is a snapshot. */
	return 0;
}

/* Begins stopping a session: it takes no more work; nothing of it runs between submissions. */
static int
sched_stop_begin(
	void *device,
	void *opaque,
	int error)
{
	struct mali_session *session;

	/* No new work from now on. */
	(void)device;
	(void)error;
	session = opaque;
	session->stopping = true;

	/* Succeeded: the stop has begun. */
	return 0;
}

/*
 * Reports a session stopped: a submission holds the device's lock until
 * its work ended or its group was stopped, so taking the lock proves no
 * work of the session runs; EAGAIN while one holds it.
 */
static int
sched_stop_poll(
	void *device,
	void *opaque)
{
	struct mali_render *render;
	int locked;
	int error;

	/* A submission in progress holds the lock: not yet (the poll does not wait). */
	render = device;
	(void)opaque;
	locked = mutex_trylock(&render->device->lock);
	if (locked == 0)
		return EAGAIN;

	error = 0;
	if (render->faulted)
		error = EIO;

	mutex_unlock(&render->device->lock);

	if (error != 0)
		return error;

	/* Succeeded: nothing of the session runs. */
	return 0;
}

/* Marks the node faulted: it takes no new session and no new work. */
static void
sched_fault(
	void *device,
	int error)
{
	struct mali_render *render;

	/* Says why once. */
	render = device;
	if (!render->faulted)
		kern_logf("mali: render node faulted (%d)\n", error);
	render->faulted = true;
}

/* Starts the node's group again once every old owner has gone. */
static int
sched_reset(
	void *device)
{
	struct mali_render *render;
	int error;

	/* The group stopped and started on the idle tables. */
	render = device;
	mutex_lock(&render->device->lock);

	error = mali_sched_restart(render);
	if (error == 0)
		render->faulted = false;

	mutex_unlock(&render->device->lock);

	if (error != 0) {
		kern_logf("mali: render node not reset (%d)\n", error);
		return error;
	}

	/* Succeeded: the node takes sessions again. */
	kern_logf("mali: render node reset\n");
	return 0;
}

/* Quarantines one session whose stop was not confirmed: it keeps its memory until a reset. */
static int
sched_isolate(
	void *device,
	void *opaque)
{
	struct mali_session *session;

	/* Refuses everything of it from now on. */
	(void)device;
	session = opaque;
	session->isolated = true;
	session->lost = true;

	/* Succeeded: the session is isolated. */
	return 0;
}

/*
 * Binds a session's tables to the node's slot, in place of the ones
 * there.  The caller holds the device's lock and no work runs.
 */
static int
bind_session(
	struct mali_render *render,
	struct mali_session *session)
{
	struct mali_device *device;
	int error;

	/* Already bound. */
	if (render->bound == session)
		return 0;

	/* The old tables leave the slot, with the GPU's caches cleaned. */
	device = render->device;
	if (render->bound != NULL) {
		mali_as_disable(device, &render->bound->space);
	} else {
		mali_as_disable(device, &render->idle);
	}

	/* The session's tables translate from now on. */
	render->bound = NULL;
	error = mali_as_enable(device, &session->space);
	if (error != 0) {
		(void)mali_as_enable(device, &render->idle);
		return error;
	}

	/* Succeeded: the group's work translates through the session's space. */
	render->bound = session;
	return 0;
}

/* Runs a frame through the session's executor, made by the first frame. */
static int
run_frame(
	struct mali_session *session,
	const void *wire,
	uint32_t bytes)
{
	int error;

	/* The executor's session. */
	if (session->vk == NULL) {
		error = mali_vk_open(session, &session->vk);
		if (error != 0)
			return error;
	}

	/* The frame; the log names a refused command. */
	error = mali_vk_execute(session->vk, wire, bytes);
	if (error != 0) {
		kern_logf("mali: vk: command frame failed opcode=%u error=%d bytes=%u\n",
			  (unsigned)mali_vk_failed_opcode(session->vk),
			  error,
			  (unsigned)bytes);
		return error;
	}

	/* Succeeded: the frame ran. */
	return 0;
}

/* Finds the index of the marker a token names among the session's; ESTALE for another pointer. */
static int
marker_index(
	const struct mali_session *session,
	const void *token,
	unsigned *index)
{
	unsigned at;

	/* Compares the token with each marker before using it. */
	for (at = 0; at < MALI_RENDER_MARKERS; at++) {
		if (token == (const void *)&session->markers[at]) {
			*index = at;
			return 0;
		}
	}

	/* No marker of the session's. */
	return ESTALE;
}

/* Reports whether the node or the session takes no work. */
static bool
refused(
	struct mali_render *render,
	struct mali_session *session)
{
	/* The node faulted. */
	if (render->faulted)
		return true;

	/* The session was lost, is stopping or isolated. */
	if (session->lost || session->stopping || session->isolated)
		return true;

	/* Both take work. */
	return false;
}

/* Stores a 32-bit word into bytes at an offset, lowest byte first. */
static void
put32(
	uint8_t *bytes,
	uint32_t at,
	uint32_t value)
{
	bytes[at] = (uint8_t)value;
	bytes[at + 1U] = (uint8_t)(value >> 8);
	bytes[at + 2U] = (uint8_t)(value >> 16);
	bytes[at + 3U] = (uint8_t)(value >> 24);
}
