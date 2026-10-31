/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR render node's Vulkan transport: the protocol's capset, the
 * command frames, and the supervised jobs libvulkan completes its fences
 * with.
 *
 * A command frame runs at once, in the session's ioctl, through the
 * executor (vk-stream.c); a submission in it has the core run its work
 * and waits for it.  The frame's callback then goes into the session's
 * ring after every native command before it, so it is delivered in order.
 * A job marker is reserved in the ring (it counts against the ring's room
 * until it is committed or cancelled) and, once committed, follows the
 * work the session submitted before it.
 *
 * Lock order as in render-queue.c: the core's lock, then the queue lock.
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

#include "drivers/gpu/powervr/render-private.h"
#include "drivers/gpu/powervr/vk/vk.h"

/*
 * The protocol's capset: its id, its bytes and the places of its words,
 * the wire version, the XML version (1.3.269), the timelines word, and
 * the vendor suffix ("SDBZ") with its flags: opaque sharing, strict
 * queues and checked quiescence, the profile libvulkan requires before
 * it creates a device on a native renderer.
 *
 * Values referenced: zedBSD userland/desktop/libvulkan/context.c (the
 * capset the transport asks for and the words it reads) and
 * userland/desktop/libvulkan/device.c (the profile a device needs).
 */
#define CAPSET_ID			4U
#define CAPSET_BYTES			168U
#define CAPSET_WIRE_AT			0U
#define CAPSET_XML_AT			4U
#define CAPSET_TIMELINES_AT		152U
#define CAPSET_MAGIC_AT			160U
#define CAPSET_FLAGS_AT			164U
#define CAPSET_WIRE_VERSION		1U
#define CAPSET_XML_VERSION		((1U << 22) | (3U << 12) | 269U)
#define CAPSET_VENDOR_MAGIC		0x5a424453U
#define CAPSET_VENDOR_FLAGS		7U

/* The timelines a frame or a job may name (0 is the context's own). */
#define RUNTIME_TIMELINES		64U

static int runtime_capset(void *device, void *session, struct gpu_capset *capset);
static int runtime_command(void *device, void *session, const void *wire, uint32_t bytes);
static int runtime_reserve(void *device, void *session, uint32_t timeline, struct drv_gpu_completion *completion, void **token);
static int runtime_commit(void *device, void *session, void *token, struct drv_gpu_completion *completion);
static int runtime_cancel(void *device, void *session, void *token, struct drv_gpu_completion *completion, unsigned fault);
static int runtime_capacity(void *device, void *session, uint32_t timeline, unsigned *available);
static int refusal(struct powervr_render *render, struct powervr_session *session, unsigned room);
static int run_frame(struct powervr_session *session, const void *wire, uint32_t bytes);
static int marker_index(const struct powervr_session *session, const void *token, unsigned *index);
static bool timeline_owned(const struct powervr_session *session, uint32_t timeline);
static void put32(uint8_t *bytes, uint32_t at, uint32_t value);

/* The supervised-job operations; the table lives as long as the kernel. */
static const struct drv_gpu_job_ops job_operations = {
	runtime_reserve,
	runtime_commit,
	runtime_cancel,
	runtime_capacity,
	NULL
};

/*
 * Binds the capset, the synchronous frame and the supervised jobs to the
 * node's table, with the capabilities they make good.
 */
void
powervr_render_runtime_bind(
	struct powervr_render *render)
{
	/* The operations and the capabilities they make good. */
	render->ops.get_capset = runtime_capset;
	render->ops.command = runtime_command;
	render->ops.jobs = &job_operations;
	render->ops.capabilities |= GPU_CAP_CAPSET | GPU_CAP_COMMAND | GPU_CAP_JOB | GPU_CAP_JOB_CAPACITY;
}

/*
 * Takes one command frame (or an empty marker frame): runs it at once.
 * A command frame's callback says the frame was decoded and its reply
 * written, which it is on return, so it completes at once (a callback
 * held until the GPU finished the session's work made every
 * transaction, vkQueueSubmit's among them, wait for the GPU); an entry
 * without a callback stays in the ring after the session's work, so the
 * work the frame kicked still counts as in flight (resource_unmake keeps
 * what it reads mapped).  A frame that failed, or one after a callback
 * of the session that carries an error, puts its callback in the ring
 * instead, with the frame's error, so errors are seen in order; so does
 * an empty marker frame, a fence after the session's work.  A
 * submission that was abandoned also reports the session lost, so the
 * core stops it.
 */
int
powervr_render_frame(
	struct powervr_render *render,
	struct powervr_session *session,
	const void *command,
	uint32_t bytes,
	uint32_t flags,
	uint32_t timeline,
	struct drv_gpu_completion *completion)
{
	unsigned long enabled;
	unsigned pending;
	int error;

	/* Only the context's fence flag; a timeline only with it. */
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

	/* A stopped or lost session takes nothing; the ring must have room for the callback. */
	error = refusal(render, session, 1);
	if (error != 0)
		return error;

	/* Runs the frame; its error goes with its callback. */
	error = 0;
	if (bytes != 0U)
		error = run_frame(session, command, bytes);

	/* The callback at once when nothing holds it back, else after everything the session sent the core. */
	enabled = spin_lock_irqsave(&render->queue_lock);

	pending = session->pending_errors;

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	if (bytes != 0U && error == 0 && pending == 0U) {
		powervr_session_pend(render, session, session->last_kind, session->last_value, NULL, 0, POWERVR_NO_MARKER);
		drv_gpu_complete(completion, 0);
	} else {
		powervr_session_pend(render, session, session->last_kind, session->last_value, completion, error, POWERVR_NO_MARKER);
	}

	/* Work the core may still run: the core stops the session. */
	if (session->vk != NULL && session->vk->abandoned)
		drv_gpu_report_session_error(render->device, session, EIO);

	/* Succeeded: the callback completed, or is held until the session's work before it retired. */
	return 0;
}

/* Reports the protocol's capset: wire 1, XML 1.3.269, timelines, the vendor suffix and its flags. */
static int
runtime_capset(
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

/*
 * Runs one frame at once; EAGAIN while a callback of the session that
 * carries an error is still in the ring (a frame may not overtake it).
 * Callbacks that only wait for the GPU to complete may be overtaken.
 */
static int
runtime_command(
	void *device,
	void *opaque,
	const void *wire,
	uint32_t bytes)
{
	struct powervr_render *render;
	struct powervr_session *session;
	unsigned long enabled;
	unsigned pending;
	int error;

	/* Whole words, within the command's bound. */
	render = device;
	session = opaque;
	if (wire == NULL || bytes == 0U || bytes > GPU_COMMAND_MAX || (bytes & 3U) != 0U)
		return EINVAL;

	/* A stopped or lost session takes nothing; earlier callbacks go first. */
	error = refusal(render, session, 0);
	if (error != 0)
		return error;

	/*
	 * Callbacks that only wait for the GPU's work to complete may be
	 * overtaken (a submission returns once it kicked its work); one that
	 * carries an error may not, so its error is seen first.
	 */
	enabled = spin_lock_irqsave(&render->queue_lock);

	pending = session->pending_errors;

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	if (pending != 0U)
		return EAGAIN;

	/* Runs the frame. */
	error = run_frame(session, wire, bytes);
	if (error != 0)
		kern_logf("powervr: frame failed (%d)\n", error);

	/* Work the core may still run: the core stops the session. */
	if (session->vk != NULL && session->vk->abandoned)
		drv_gpu_report_session_error(render->device, session, EIO);
	if (error != 0)
		return error;

	/* Succeeded: the frame ran. */
	return 0;
}

/*
 * Reserves a job marker on a timeline the session's queue owns.  EAGAIN
 * when the ring has no room (the producer waits for capacity), EIO once
 * the device or the session is lost.
 */
static int
runtime_reserve(
	void *device,
	void *opaque,
	uint32_t timeline,
	struct drv_gpu_completion *completion,
	void **token)
{
	struct powervr_render *render;
	struct powervr_session *session;
	unsigned long enabled;
	unsigned index;
	bool owned;
	int error;

	/* A timeline of the session's queue. */
	render = device;
	session = opaque;
	*token = NULL;
	owned = timeline_owned(session, timeline);
	if (!owned)
		return EINVAL;

	/* A free marker, while the ring has room for it. */
	enabled = spin_lock_irqsave(&render->queue_lock);

	error = 0;
	if (render->faulted || session->stopping || session->lost || session->isolated) {
		error = EIO;
		powervr_render_refusal_log(render, session, "reserve");
	} else if (session->pending_count + session->reserved >= POWERVR_RENDER_PENDING) {
		error = EAGAIN;
	}

	/* The first free marker is the session's, reserved. */
	if (error == 0) {
		error = EAGAIN;
		for (index = 0; index < POWERVR_RENDER_PENDING; index++) {
			if (session->markers[index].state == POWERVR_MARKER_FREE) {
				session->markers[index].state = POWERVR_MARKER_RESERVED;
				session->markers[index].completion = completion;
				session->reserved++;
				*token = &session->markers[index];
				error = 0;
				break;
			}
		}
	}

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	/* No marker. */
	if (error != 0)
		return error;

	/* Succeeded: the token names the marker. */
	return 0;
}

/* Commits a reserved marker: it follows the work the session submitted before it. */
static int
runtime_commit(
	void *device,
	void *opaque,
	void *token,
	struct drv_gpu_completion *completion)
{
	struct powervr_render *render;
	struct powervr_session *session;
	unsigned long enabled;
	unsigned index;
	int error;

	/* The marker the token names. */
	render = device;
	session = opaque;
	error = marker_index(session, token, &index);
	if (error != 0)
		return error;

	/* A reservation of this completion, on a device that runs. */
	enabled = spin_lock_irqsave(&render->queue_lock);

	error = 0;
	if (session->markers[index].state != POWERVR_MARKER_RESERVED || session->markers[index].completion != completion) {
		error = ESTALE;
	} else if (render->faulted) {
		error = EIO;
		powervr_render_refusal_log(render, session, "commit");
	} else {
		session->markers[index].state = POWERVR_MARKER_COMMITTED;
		session->reserved--;
	}

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	if (error != 0)
		return error;

	/* Into the ring after the session's work; the delivery frees the marker. */
	powervr_session_pend(render, session, session->last_kind, session->last_value, completion, 0, index);

	/* Succeeded: the marker completes in order. */
	return 0;
}

/*
 * Withdraws a reservation (normal: not committed, no completion) or ends
 * an uncertain one with EIO (fault: put in the ring with the error).
 */
static int
runtime_cancel(
	void *device,
	void *opaque,
	void *token,
	struct drv_gpu_completion *completion,
	unsigned fault)
{
	struct powervr_render *render;
	struct powervr_session *session;
	unsigned long enabled;
	unsigned index;
	unsigned state;
	int error;

	/* Normal or fault only, of a marker the token names. */
	render = device;
	session = opaque;
	if (fault > 1U)
		return EINVAL;
	error = marker_index(session, token, &index);
	if (error != 0)
		return error;

	/* The marker's state, and what the cancel does to it. */
	enabled = spin_lock_irqsave(&render->queue_lock);

	state = session->markers[index].state;
	error = 0;
	if (state == POWERVR_MARKER_FREE || session->markers[index].completion != completion) {
		error = ESTALE;
	} else if (state == POWERVR_MARKER_RESERVED && fault != 0U) {
		session->markers[index].state = POWERVR_MARKER_COMMITTED;
		session->reserved--;
	} else if (state == POWERVR_MARKER_RESERVED) {
		session->markers[index].state = POWERVR_MARKER_FREE;
		session->markers[index].completion = NULL;
		session->reserved--;
	} else if (fault == 0U) {
		error = ESTALE;
	}

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	if (error != 0)
		return error;

	/* A faulted reservation ends with the error, in order. */
	if (state == POWERVR_MARKER_RESERVED && fault != 0U)
		powervr_session_pend(render, session, session->last_kind, session->last_value, completion, EIO, index);

	/* Succeeded: the reservation is withdrawn or ended. */
	return 0;
}

/* Reports how many markers a reservation on the timeline could take now. */
static int
runtime_capacity(
	void *device,
	void *opaque,
	uint32_t timeline,
	unsigned *available)
{
	struct powervr_render *render;
	struct powervr_session *session;
	unsigned long enabled;
	bool owned;
	int error;

	/* A timeline of the session's queue. */
	render = device;
	session = opaque;
	*available = 0;
	owned = timeline_owned(session, timeline);
	if (!owned)
		return EINVAL;

	/* The ring's room, unless the device faulted. */
	enabled = spin_lock_irqsave(&render->queue_lock);

	error = 0;
	if (render->faulted)
		error = EIO;
	else
		*available = POWERVR_RENDER_PENDING - session->pending_count - session->reserved;

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	if (error != 0)
		return error;

	/* Succeeded: the count is a snapshot, not a promise. */
	return 0;
}

/*
 * Says why a session takes no new work: EIO when the device or the
 * session is lost or stopping, EBUSY when the ring has less than room
 * free.
 */
static int
refusal(
	struct powervr_render *render,
	struct powervr_session *session,
	unsigned room)
{
	unsigned long enabled;
	int error;

	/* The flags and the ring's use, under the queue lock. */
	enabled = spin_lock_irqsave(&render->queue_lock);

	error = 0;
	if (render->faulted || session->stopping || session->lost || session->isolated) {
		error = EIO;
		powervr_render_refusal_log(render, session, "submit");
	} else if (session->pending_count + session->reserved + room > POWERVR_RENDER_PENDING) {
		error = EBUSY;
	}

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	if (error != 0)
		return error;

	/* Succeeded: the session takes work. */
	return 0;
}

/* Runs a frame through the session's executor, made by the first frame. */
static int
run_frame(
	struct powervr_session *session,
	const void *wire,
	uint32_t bytes)
{
	int error;

	/* The executor's session. */
	if (session->vk == NULL) {
		error = powervr_vk_open(session, &session->vk);
		if (error != 0)
			return error;
	}

	/* The frame; the log names a refused command. */
	error = powervr_vk_execute(session->vk, wire, bytes);
	if (error != 0) {
		kern_logf("powervr: vk: command frame failed opcode=%u error=%d bytes=%u\n", (unsigned)session->vk->failed_opcode, error, (unsigned)bytes);
		return error;
	}

	/* Succeeded: the frame ran. */
	return 0;
}

/* Finds the index of the marker a token names among the session's; ESTALE for another pointer. */
static int
marker_index(
	const struct powervr_session *session,
	const void *token,
	unsigned *index)
{
	unsigned at;

	/* Compares the token with each marker before using it. */
	for (at = 0; at < POWERVR_RENDER_PENDING; at++) {
		if (token == (const void *)&session->markers[at]) {
			*index = at;
			return 0;
		}
	}

	/* No marker of the session's. */
	return ESTALE;
}

/* Reports whether the session's queue completes jobs on the timeline. */
static bool
timeline_owned(
	const struct powervr_session *session,
	uint32_t timeline)
{
	uint64_t timelines;

	/* Timelines 1 to 63, of a session with an executor. */
	if (timeline == 0U || timeline >= RUNTIME_TIMELINES || session->vk == NULL)
		return false;

	/* The executor records the timelines its queue was made with. */
	timelines = __atomic_load_n(&session->vk->timelines, __ATOMIC_ACQUIRE);
	if ((timelines & (1ULL << timeline)) == 0U)
		return false;

	/* Succeeded: the timeline is the session's. */
	return true;
}

/* Stores a 32-bit word into bytes at an offset, lowest byte first. */
static void
put32(
	uint8_t *bytes,
	uint32_t at,
	uint32_t value)
{
	/* One byte at a time. */
	bytes[at] = (uint8_t)value;
	bytes[at + 1U] = (uint8_t)(value >> 8);
	bytes[at + 2U] = (uint8_t)(value >> 16);
	bytes[at + 3U] = (uint8_t)(value >> 24);
}
