/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU device's Vulkan transport (nvrtx.vk=1).
 *
 * With the executor bound, the node is a renderer libvulkan opens: it
 * reports the protocol's capset, runs command frames (synchronously, or
 * queued with a completion), and takes supervised jobs whose completion
 * markers follow the frames queued before them.  One worker thread runs
 * the queue in order: a frame through the executor under the node's lock,
 * a marker by completing it.  The executor's submissions do not wait for
 * the GPU, so a marker whose session's newest run has not ended yet is
 * parked, and the completer thread completes the parked markers in order
 * as their runs end, waiting without the node's lock (a run that does not
 * end in time completes its marker with EIO and loses the session's runs).
 * A marker committed while its session has nothing queued is taken at once
 * on the committing thread, as the worker would take it.
 * A fault stops new work: frames and markers still queued or parked
 * complete with EIO.
 *
 * queue_lock (a spinlock) guards the queue, the parked markers, the
 * markers, every session's pending and queued counts and the fault flag;
 * the node's lock (a mutex) is taken only outside it, around the executor.
 *
 * A session's first frame gives it a channel of its own on the GPU (made
 * under the node's channel_lock, before the node's lock is taken, as it
 * calls the GSP), or the driver's work channel to share when none can be
 * made; its close frees that channel.
 *
 * The recovery: a run that does not end in its time wakes the reaper
 * thread (and the reaper looks every period on its own).  It picks the
 * channel that holds the graphics engine up (recover.c), loses its
 * session (or every session sharing the work channel) and stops it on the
 * GPU: the stop, else the GSP's error report on it, else its reset, then
 * its free.  The GSP's error reports on a channel lose its session too.
 * Other channels' runs then go on, their time counted from the stop's end.
 * The reaper alone stops channels; the core's isolate only reads their
 * state.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/clock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>
#include <uapi/gpu-job.h>

#include "drivers/gpu/nvrtx/nvrtx-node.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-vk.h"

/* The protocol's capset: its id, its bytes, the XML version (1.3.269), and the vendor suffix ("SDBZ") with its flags. */
#define RUNTIME_CAPSET_ID		4U
#define RUNTIME_CAPSET_BYTES		168U
#define RUNTIME_CAPSET_WIRE_AT		0U
#define RUNTIME_CAPSET_XML_AT		4U
#define RUNTIME_CAPSET_TIMELINES_AT	152U
#define RUNTIME_CAPSET_MAGIC_AT		160U
#define RUNTIME_CAPSET_FLAGS_AT		164U
#define RUNTIME_WIRE_VERSION		1U
#define RUNTIME_XML_VERSION		((1U << 22) | (3U << 12) | 269U)
#define RUNTIME_VENDOR_MAGIC		0x5a424453U

/* The flags: opaque sharing, strict queues, checked quiescence (the profile libvulkan pairs with). */
#define RUNTIME_VENDOR_FLAGS		7U

/*
 * The native record after the vendor suffix, sent when the reply holds it:
 * the capset's bytes with it, its tag and its flags (bit 0: an H.264
 * decode family, when the device decodes video), as libvulkan reads them.
 */
#define RUNTIME_NATIVE_CAPSET_BYTES	176U
#define RUNTIME_NATIVE_MAGIC_AT		168U
#define RUNTIME_NATIVE_FLAGS_AT		172U
#define RUNTIME_NATIVE_MAGIC		0x5a4e4154U
#define RUNTIME_NATIVE_VIDEO_H264	0x1U

/*
 * The native flag of a command run at once on the caller's thread
 * (GPU_COMMAND, runtime_command), its replies written when the call
 * returns, as libvulkan reads it (VULKAN_NATIVE_CAPSET_SYNC_COMMAND).
 */
#define RUNTIME_NATIVE_SYNC_COMMAND	0x2U

/* The timelines a queue may name (1 to 63; 0 is the context's own). */
#define RUNTIME_TIMELINES		64U

/*
 * How often the reaper looks at the channels on its own, how long it waits
 * for the GSP's error report on a channel whose stop failed and how often
 * it looks for it.
 */
#define RUNTIME_REAPER_PERIOD_MS	250U
#define RUNTIME_REPORT_WAIT_US		500000U
#define RUNTIME_REPORT_STEP_US		10000U

/* How long the stop for the reboot waits for a stop the reaper is making, and its step. */
#define RUNTIME_REAPER_STOP_US		15000000U
#define RUNTIME_REAPER_STOP_STEP_US	10000U

/*
 * The GSP's error report that the GPU fell off the bus: the whole device is
 * lost.
 *
 * Value referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/nverror.h (Xid 79).
 */
#define RUNTIME_RC_OFF_THE_BUS		79U

/* The views the reaper takes: one per session's number, then the shared work channel. */
#define RUNTIME_VIEWS			(NVRTX_WORK_SESSION_CHANNELS + 1U)

/* What a queue entry is. */
#define RUNTIME_ENTRY_FRAME		1U
#define RUNTIME_ENTRY_MARKER		2U

static int runtime_capset(void *private_data, void *session, struct gpu_capset *capset);
static int runtime_command(void *private_data, void *session, const void *wire, uint32_t bytes);
static int runtime_submit(void *private_data, void *session, const void *wire, uint32_t bytes, uint32_t flags, uint32_t timeline, struct drv_gpu_completion *completion);
static void runtime_drain(void *private_data, void *session);
static int runtime_reserve(void *private_data, void *session, uint32_t timeline, struct drv_gpu_completion *completion, void **token);
static int runtime_commit(void *private_data, void *session, void *token, struct drv_gpu_completion *completion);
static int runtime_cancel(void *private_data, void *session, void *token, struct drv_gpu_completion *completion, unsigned fault);
static int runtime_capacity(void *private_data, void *session, uint32_t timeline, unsigned *available);
static int runtime_job_command(void *private_data, void *session, const void *wire, uint32_t bytes, uint32_t *outcome);
static int runtime_stop_begin(void *private_data, void *session, int error);
static int runtime_stop_poll(void *private_data, void *session);
static void runtime_fault(void *private_data, int error);
static int runtime_isolate(void *private_data, void *session);
static void runtime_reaper(void *argument);
static void runtime_reap(struct nvrtx_node *node);
static int runtime_pick(struct nvrtx_node *node, struct nvrtx_work_channel **picked);
static void runtime_kill(struct nvrtx_node *node, struct nvrtx_work_channel *channel);
static void runtime_channel_error(void *argument, uint32_t chid, uint32_t type);
static bool runtime_session_stopped(struct nvrtx_node *node, struct nvrtx_session *session);
static void runtime_worker(void *argument);
static void runtime_completer(void *argument);
static int runtime_lane_start(struct nvrtx_lane *lane);
static struct nvrtx_lane *runtime_lane(struct nvrtx_node *node, struct nvrtx_session *session);
static void runtime_wake_lanes(struct nvrtx_node *node);
static bool runtime_park(struct nvrtx_node *node, struct nvrtx_node_entry *entry);
static void runtime_dequeued(struct nvrtx_node *node, struct nvrtx_session *owner);
static void runtime_retire(struct nvrtx_node *node, struct nvrtx_node_entry *entry);
static int runtime_execute(struct nvrtx_node *node, struct nvrtx_session *session, const void *wire, uint32_t bytes);
static void runtime_channel(struct nvrtx_node *node, struct nvrtx_session *session);
static void runtime_enqueue(struct nvrtx_node *node, struct nvrtx_node_entry *entry);
static struct nvrtx_node_entry *runtime_marker(struct nvrtx_node *node, struct nvrtx_session *session, void *token, struct drv_gpu_completion *completion);
static unsigned runtime_free_markers(const struct nvrtx_node *node);
static bool runtime_timeline_owned(struct nvrtx_session *session, uint32_t timeline);

/* The queued-command operations: submit and drain. */
static const struct drv_gpu_command_ops runtime_commands = {
	runtime_submit,
	runtime_drain
};

/* The supervised-job operations. */
static const struct drv_gpu_job_ops runtime_jobs = {
	runtime_reserve,
	runtime_commit,
	runtime_cancel,
	runtime_capacity,
	runtime_job_command
};

/* The recovery operations: a session's stop and its poll, the device's fault and a session's isolation (no reset). */
static const struct drv_gpu_recovery_ops runtime_recovery = {
	runtime_stop_begin,
	runtime_stop_poll,
	runtime_fault,
	NULL,
	runtime_isolate
};

/*
 * Binds the renderer's operations to the node before it is registered: the
 * queue, its lock and wait queues, and the capabilities they make good.
 */
void
nvrtx_runtime_bind(
	struct nvrtx_node *node)
{
	struct nvrtx_lane *lane;
	uint32_t index;
	bool wanted;

	/* The lanes (the main one and one per session's number), their lock and the wait queues. */
	spin_init(&node->queue_lock, LOCK_RANK_DEVICE, "nvrtx queue");
	waitq_init(&node->retired, "nvrtx queue retired");
	for (index = 0U; index <= NVRTX_WORK_SESSION_CHANNELS; index++) {
		lane = &node->main_lane;
		if (index < NVRTX_WORK_SESSION_CHANNELS)
			lane = &node->lanes[index];
		lane->node = node;
		lane->head = NULL;
		lane->tail = NULL;
		lane->parked_head = NULL;
		lane->parked_tail = NULL;
		lane->started = 0;
		waitq_init(&lane->available, "nvrtx lane available");
		waitq_init(&lane->parked_more, "nvrtx lane parked");
	}

	/* The sessions' channels: none made yet, the first number taken first, unless the boot asked for none. */
	(void)mutex_init(&node->channel_lock, LOCK_RANK_DEVICE, "nvrtx channels");
	node->chid_used = 0U;
	node->chid_last = NVRTX_WORK_SESSION_CHANNELS - 1U;
	wanted = nvrtx_stage_channels_wanted();
	node->channels_wanted = 0;
	if (wanted)
		node->channels_wanted = 1;

	/* The recovery: its wait queue, and whether it stops channels (unless the boot asked it not to). */
	waitq_init(&node->reaper_wake, "nvrtx reaper");
	wanted = nvrtx_stage_kill_wanted();
	node->kills_wanted = 0;
	if (wanted)
		node->kills_wanted = 1;

	/* The operations and the capabilities they make good. */
	node->ops.get_capset = runtime_capset;
	node->ops.command = runtime_command;
	node->ops.commands = &runtime_commands;
	node->ops.jobs = &runtime_jobs;
	node->ops.recovery = &runtime_recovery;
	node->ops.capabilities |= GPU_CAP_CAPSET | GPU_CAP_COMMAND | GPU_CAP_NOTIFICATION | GPU_CAP_JOB | GPU_CAP_JOB_CAPACITY;
	node->renderer = 1;
}

/*
 * Starts the worker and the completer once the node is registered; they
 * run for the node's life.  Without them nothing queued would complete, so
 * a failure faults the renderer (every queued operation then answers EIO).
 */
void
nvrtx_runtime_start(
	struct nvrtx_node *node)
{
	struct thread *worker;
	struct thread *completer;
	struct thread *reaper;
	unsigned long enabled;
	int error;

	/* The reaper, the completer of the parked markers, then the worker. */
	error = kthread_create(runtime_reaper, node, SCHED_PRIORITY_DEFAULT, &reaper);
	if (error == 0) {
		reaper->detached = 1;
		thread_start(reaper);

		/* The GSP's error reports on channels reach the recovery from now on. */
		__atomic_store_n(&node->gpu->channel_error_argument, node, __ATOMIC_RELEASE);
		__atomic_store_n(&node->gpu->channel_error, runtime_channel_error, __ATOMIC_RELEASE);
		error = kthread_create(runtime_completer, &node->main_lane, SCHED_PRIORITY_DEFAULT, &completer);
	}
	if (error == 0) {
		completer->detached = 1;
		thread_start(completer);
		error = kthread_create(runtime_worker, &node->main_lane, SCHED_PRIORITY_DEFAULT, &worker);
	}
	if (error == 0) {
		worker->detached = 1;
		thread_start(worker);
		node->main_lane.started = 1;
		return;
	}

	/* No worker or no completer: the renderer takes nothing. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	node->faulted = 1;

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Says which thread could not be made. */
	nvrtx_stage_mark("renderer: no renderer worker, completer or reaper (%d)", error);
}

/*
 * Stops the renderer for the reboot: nothing new is taken and what is
 * queued completes with EIO, as after a fault, but without a fault's
 * report.  The worker thread runs on, idle, until the reset.
 */
void
nvrtx_runtime_stop(
	struct nvrtx_node *node)
{
	unsigned long enabled;
	uint64_t start;
	uint64_t spent;
	int busy;

	/* A node that is no renderer has no queue. */
	if (node->renderer == 0)
		return;

	/* faulted refuses every later frame, marker and reservation; the worker completes the rest with EIO; the reaper stops nothing more. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	node->faulted = 1;
	node->reaper_stop = 1;
	runtime_wake_lanes(node);

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* A stop the reaper is making ends before the GSP is stopped (its calls may take their whole time). */
	start = nvrtx_now_us();
	for (;;) {
		busy = __atomic_load_n(&node->reaper_busy, __ATOMIC_ACQUIRE);
		if (busy == 0)
			break;
		spent = nvrtx_now_us() - start;
		if (spent > RUNTIME_REAPER_STOP_US) {
			nvrtx_stage_mark("unload shutdown: the reaper's stop did not end");
			break;
		}

		/* Gives the reaper a step to finish. */
		kern_usleep_range(RUNTIME_REAPER_STOP_STEP_US, RUNTIME_REAPER_STOP_STEP_US * 2U);
	}

	nvrtx_stage_mark("unload shutdown: the renderer takes no more work");
}

/*
 * Closes a session's channel once its executor session is closed (its
 * runs ended, or were given up): a channel of its own is freed and its
 * number free again; one the resource manager would not free, or one of
 * an isolated session, stays the device's, with its number.  The shared
 * work channel stays.
 */
void
nvrtx_runtime_session_close(
	struct nvrtx_node *node,
	struct nvrtx_session *session)
{
	struct nvrtx_work_channel *channel;
	unsigned long enabled;
	uint32_t number;
	int quarantined;
	int error;

	/* A session that never ran a frame, or one that shares the work channel, has none of its own. */
	channel = session->channel;
	if (channel == NULL || session->shared != 0)
		return;
	number = channel->chid - NVRTX_WORK_SESSION_CHID_FIRST;

	/*
	 * Under the lock of the channels' making (the reaper holds it while it
	 * looks and stops): the GSP's reports no longer name the session, then
	 * the channel is freed and its number free again, unless the core
	 * isolated the session (its channel stays the device's).
	 */
	mutex_lock(&node->channel_lock);

	enabled = spin_lock_irqsave(&node->queue_lock);

	node->channels[number] = NULL;
	quarantined = session->quarantined;

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* An isolated session's channel stays the device's; any other is closed. */
	error = EBUSY;
	if (quarantined == 0)
		error = nvrtx_work_channel_close(node->gpu, channel);

	/* The number is free again only once the channel is gone. */
	if (error == 0)
		node->chid_used &= ~(1U << number);

	mutex_unlock(&node->channel_lock);

	/* A channel that stays keeps its record, as its memory stays. */
	session->channel = NULL;
	if (error != 0)
		return;

	/* Succeeded: the channel is gone. */
	kern_free(channel);
}

/* Wakes the reaper: a run did not end in its time. */
void
nvrtx_runtime_reaper_wake(
	struct nvrtx_node *node)
{
	unsigned long enabled;

	/* The reaper looks at the channels at once. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	waitq_wake_all(&node->reaper_wake);

	spin_unlock_irqrestore(&node->queue_lock, enabled);
}

/* Reports the protocol's capset: wire 1, XML 1.3.269, timelines, the vendor suffix and its flags. */
static int
runtime_capset(
	void *private_data,
	void *session,
	struct gpu_capset *capset)
{
	struct nvrtx_node *node;
	uint32_t flags;
	bool video;

	UNUSED_PARAMETER(session);

	/* Takes the node the capset is asked of. */
	node = private_data;

	/* Only the protocol's capset, version 0, in a reply that holds it. */
	if (capset->capset_id != RUNTIME_CAPSET_ID || capset->capset_version != 0U)
		return ENOTSUP;
	if (capset->capacity < RUNTIME_CAPSET_BYTES || capset->capacity > GPU_CAPSET_MAX)
		return EINVAL;

	/* Every word not named is zero. */
	kern_memset(capset->data, 0, sizeof(capset->data));
	nvrtx_put32(capset->data, RUNTIME_CAPSET_WIRE_AT, RUNTIME_WIRE_VERSION);
	nvrtx_put32(capset->data, RUNTIME_CAPSET_XML_AT, RUNTIME_XML_VERSION);
	nvrtx_put32(capset->data, RUNTIME_CAPSET_TIMELINES_AT, 1U);
	nvrtx_put32(capset->data, RUNTIME_CAPSET_MAGIC_AT, RUNTIME_VENDOR_MAGIC);
	nvrtx_put32(capset->data, RUNTIME_CAPSET_FLAGS_AT, RUNTIME_VENDOR_FLAGS);
	capset->bytes = RUNTIME_CAPSET_BYTES;

	/*
	 * The native record, when the reply holds it: commands run at once on
	 * the caller's thread, and the video decode family when there is one.
	 */
	if (capset->capacity >= RUNTIME_NATIVE_CAPSET_BYTES) {
		flags = RUNTIME_NATIVE_SYNC_COMMAND;
		video = nvrtx_node_video_available(node);
		if (video)
			flags |= RUNTIME_NATIVE_VIDEO_H264;
		nvrtx_put32(capset->data, RUNTIME_NATIVE_MAGIC_AT, RUNTIME_NATIVE_MAGIC);
		nvrtx_put32(capset->data, RUNTIME_NATIVE_FLAGS_AT, flags);
		capset->bytes = RUNTIME_NATIVE_CAPSET_BYTES;
	}

	/* Succeeded: the capset is reported. */
	return 0;
}

/*
 * Runs one frame at once on the caller's thread; EAGAIN while frames or
 * markers of the session are queued (a frame may not overtake them; a
 * parked marker does not hold it back).  The frame counts as the
 * session's pending work while it runs, so a stop or a drain waits for it.
 */
static int
runtime_command(
	void *private_data,
	void *session,
	const void *wire,
	uint32_t bytes)
{
	struct nvrtx_node *node;
	struct nvrtx_session *owner;
	unsigned long enabled;
	uint32_t queued;
	int faulted;
	int error;

	node = private_data;
	owner = session;

	/* Whole words, within the command's bound. */
	if (bytes == 0U || bytes > GPU_COMMAND_MAX || (bytes & 3U) != 0U)
		return EINVAL;

	/* Looks at the session's queued work and the fault; a frame that runs counts as pending. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	queued = owner->queued;
	faulted = node->faulted;
	if (owner->stopping != 0)
		faulted = 1;
	if (faulted == 0 && queued == 0U)
		owner->pending++;

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* A faulted device, or a session being stopped, takes nothing; queued work goes first. */
	if (faulted != 0)
		return EIO;
	if (queued != 0U)
		return EAGAIN;

	/* Runs the frame through the executor. */
	error = runtime_execute(node, owner, wire, bytes);

	/* The frame has ended: the drains look again. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	owner->pending--;
	waitq_wake_all(&node->retired);

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Reports a frame that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the frame ran. */
	return 0;
}

/*
 * Queues one frame (a copy) with its completion, which the worker
 * completes once the frame has run.  A frame of no bytes is a marker of the
 * context's fence alone.
 */
static int
runtime_submit(
	void *private_data,
	void *session,
	const void *wire,
	uint32_t bytes,
	uint32_t flags,
	uint32_t timeline,
	struct drv_gpu_completion *completion)
{
	struct nvrtx_node *node;
	struct nvrtx_node_entry *entry;
	unsigned long enabled;
	int faulted;

	node = private_data;

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
	if (bytes != 0U && wire == NULL)
		return EINVAL;

	/* The session's channel and lane before its first entry is queued. */
	runtime_channel(node, session);

	/* Allocates the entry with its copy of the frame. */
	entry = kern_calloc(1U, sizeof(*entry) + bytes);
	if (entry == NULL)
		return ENOMEM;

	entry->kind = RUNTIME_ENTRY_FRAME;
	entry->session = session;
	entry->completion = completion;
	entry->bytes = bytes;
	entry->wire = (uint8_t *)(entry + 1);
	if (bytes != 0U)
		kern_memcpy(entry->wire, wire, bytes);

	/* Queues it unless the device faulted or the session is being stopped. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	faulted = node->faulted;
	if (entry->session->stopping != 0)
		faulted = 1;
	if (faulted == 0) {
		entry->session->pending++;
		runtime_enqueue(node, entry);
	}

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* A refused frame stays the caller's: nothing will complete it. */
	if (faulted != 0) {
		kern_free(entry);
		return EIO;
	}

	/* Succeeded: the worker completes the frame. */
	return 0;
}

/* Waits until every queued frame, marker and reservation of the session has ended. */
static void
runtime_drain(
	void *private_data,
	void *session)
{
	struct nvrtx_node *node;
	struct nvrtx_session *owner;
	unsigned long enabled;
	uint64_t observed;

	node = private_data;
	owner = session;

	/* Sleeps on the retirements until the session has nothing pending. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	while (owner->pending != 0U) {
		observed = waitq_sequence(&node->retired);
		(void)waitq_sleep(&node->retired, &node->queue_lock, observed, 0U, 0U);
	}

	spin_unlock_irqrestore(&node->queue_lock, enabled);
}

/*
 * Reserves a marker for a supervised job on a timeline the session's queue
 * owns.  Returns EAGAIN when no marker is free (the producer waits for
 * capacity), EIO once the device faulted.
 */
static int
runtime_reserve(
	void *private_data,
	void *session,
	uint32_t timeline,
	struct drv_gpu_completion *completion,
	void **token)
{
	struct nvrtx_node *node;
	struct nvrtx_session *owner;
	struct nvrtx_node_entry *entry;
	unsigned long enabled;
	uint32_t slot;
	bool owned;

	node = private_data;
	owner = session;
	*token = NULL;

	/* A timeline of the session's queues. */
	if (timeline == 0U || timeline >= RUNTIME_TIMELINES)
		return EINVAL;
	owned = runtime_timeline_owned(owner, timeline);
	if (!owned)
		return EINVAL;

	/* The session's channel and lane before its first marker is queued. */
	runtime_channel(node, owner);

	/* Takes a free marker unless the device faulted or the session is being stopped. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	if (node->faulted != 0 || owner->stopping != 0) {
		spin_unlock_irqrestore(&node->queue_lock, enabled);
		return EIO;
	}

	entry = NULL;
	for (slot = 0U; slot < NVRTX_NODE_MARKERS; slot++) {
		if (node->markers[slot].state == NVRTX_ENTRY_FREE) {
			entry = &node->markers[slot];
			break;
		}
	}

	if (entry == NULL) {
		spin_unlock_irqrestore(&node->queue_lock, enabled);
		return EAGAIN;
	}

	/*
	 * The marker is the session's, reserved: the session counts it as
	 * pending until it is cancelled or its completion is delivered.
	 */
	entry->kind = RUNTIME_ENTRY_MARKER;
	entry->session = owner;
	entry->completion = completion;
	entry->error = 0;
	entry->state = NVRTX_ENTRY_RESERVED;
	owner->pending++;

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Succeeded: the token names the marker. */
	*token = entry;
	return 0;
}

/*
 * Commits a reserved marker: after the frames queued before it (the
 * worker takes it), or, when the session has nothing queued or running in
 * the worker, at once on the caller's thread as the worker would take it
 * (parked for the completer while its run has not ended), so the session's
 * next command does not find it queued and runs at once.
 */
static int
runtime_commit(
	void *private_data,
	void *session,
	void *token,
	struct drv_gpu_completion *completion)
{
	struct nvrtx_node *node;
	struct nvrtx_session *owner;
	struct nvrtx_node_entry *entry;
	unsigned long enabled;
	bool parked;
	int error;

	node = private_data;
	owner = session;

	/* Finds the reservation, unless the device faulted. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	entry = runtime_marker(node, session, token, completion);
	if (entry == NULL || entry->state != NVRTX_ENTRY_RESERVED) {
		spin_unlock_irqrestore(&node->queue_lock, enabled);
		return ESTALE;
	}

	if (node->faulted != 0) {
		spin_unlock_irqrestore(&node->queue_lock, enabled);
		return EIO;
	}

	/* Behind the session's queued work it goes to the worker. */
	if (owner->queued != 0U) {
		runtime_enqueue(node, entry);
		spin_unlock_irqrestore(&node->queue_lock, enabled);
		return 0;
	}

	/* Else it is taken here, counted as the worker's while it is (runtime_park takes it off). */
	entry->state = NVRTX_ENTRY_RUNNING;
	entry->next = NULL;
	owner->queued++;

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Parked while its run has not ended; else completed and retired now. */
	parked = runtime_park(node, entry);
	if (!parked) {
		error = nvrtx_vk_node_run_wait(owner, entry->run_fence, entry->run_us);
		drv_gpu_complete(entry->completion, error);
		runtime_retire(node, entry);
	}

	/* Succeeded: the marker completes in order. */
	return 0;
}

/*
 * Withdraws a reservation (normal: before it was committed, no completion)
 * or ends an uncertain one with EIO (fault: queued to complete with the
 * error).
 */
static int
runtime_cancel(
	void *private_data,
	void *session,
	void *token,
	struct drv_gpu_completion *completion,
	unsigned fault)
{
	struct nvrtx_node *node;
	struct nvrtx_node_entry *entry;
	unsigned long enabled;

	node = private_data;

	/* Normal or fault only. */
	if (fault > 1U)
		return EINVAL;

	/* Finds the reservation. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	entry = runtime_marker(node, session, token, completion);
	if (entry == NULL) {
		spin_unlock_irqrestore(&node->queue_lock, enabled);
		return ESTALE;
	}

	/* A fault ends a reservation not yet queued with the error, in order. */
	if (fault != 0U) {
		if (entry->state == NVRTX_ENTRY_RESERVED) {
			entry->error = EIO;
			runtime_enqueue(node, entry);
		}

		spin_unlock_irqrestore(&node->queue_lock, enabled);
		return 0;
	}

	/* A normal cancel withdraws only a reservation not yet queued. */
	if (entry->state != NVRTX_ENTRY_RESERVED) {
		spin_unlock_irqrestore(&node->queue_lock, enabled);
		return ESTALE;
	}

	/* The marker is free again; the session no longer waits for it. */
	entry->state = NVRTX_ENTRY_FREE;
	entry->session->pending--;
	entry->session = NULL;
	entry->completion = NULL;
	waitq_wake_all(&node->retired);

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Succeeded: the reservation is withdrawn without a completion (the GPU core tells capacity observers). */
	return 0;
}

/*
 * Runs a supervised job's frame at once on the caller's thread
 * (GPU_JOB_SUBMIT), as runtime_command() does, and tells its outcome from
 * the queue submissions the frame ran and the runs it put on the GPU:
 * committed when it ran at least one submission and every one succeeded;
 * else canceled when no run reached the GPU (a failed frame's error is
 * returned with it); else faulted (a submission that failed may have put
 * part of its work on the GPU).  Deferred, with nothing run, while frames
 * or markers of the session are queued.  Returns 0 or the frame's error
 * (with the outcome set), or EIO with nothing run on a faulted device.
 */
static int
runtime_job_command(
	void *private_data,
	void *session,
	const void *wire,
	uint32_t bytes,
	uint32_t *outcome)
{
	struct nvrtx_node *node;
	struct nvrtx_session *owner;
	unsigned long enabled;
	uint32_t queued;
	int faulted;
	int error;

	node = private_data;
	owner = session;
	*outcome = GPU_JOB_SUBMIT_FAULTED;

	/* Whole words, within the command's bound. */
	if (bytes == 0U || bytes > GPU_COMMAND_MAX || (bytes & 3U) != 0U)
		return EINVAL;

	/* Looks at the session's queued work and the fault; a frame that runs counts as pending. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	queued = owner->queued;
	faulted = node->faulted;
	if (owner->stopping != 0)
		faulted = 1;
	if (faulted == 0 && queued == 0U)
		owner->pending++;

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* A faulted device, or a session being stopped, takes nothing; queued work goes first. */
	if (faulted != 0)
		return EIO;
	if (queued != 0U) {
		*outcome = GPU_JOB_SUBMIT_DEFERRED;
		return 0;
	}

	/* Runs the frame, its queue submissions and runs counted from zero (nothing else runs the session's frames now). */
	owner->vk->queue_ran = 0U;
	owner->vk->queue_accepted = 0U;
	owner->vk->gpu_sends = 0U;
	error = runtime_execute(node, owner, wire, bytes);
	*outcome = GPU_JOB_SUBMIT_FAULTED;
	if (error == 0 && owner->vk->queue_ran != 0U && owner->vk->queue_accepted == owner->vk->queue_ran)
		*outcome = GPU_JOB_SUBMIT_COMMITTED;
	else if (owner->vk->gpu_sends == 0U)
		*outcome = GPU_JOB_SUBMIT_CANCELED;

	/* The frame has ended: the drains look again. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	owner->pending--;
	waitq_wake_all(&node->retired);

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Reports a frame that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the outcome is the frame's. */
	return 0;
}

/* Reports how many markers a reservation on the timeline could take now. */
static int
runtime_capacity(
	void *private_data,
	void *session,
	uint32_t timeline,
	unsigned *available)
{
	struct nvrtx_node *node;
	unsigned long enabled;
	bool owned;

	node = private_data;
	*available = 0U;

	/* A timeline of the session's queues. */
	if (timeline == 0U || timeline >= RUNTIME_TIMELINES)
		return EINVAL;
	owned = runtime_timeline_owned(session, timeline);
	if (!owned)
		return EINVAL;

	/* The free markers, unless the device faulted. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	if (node->faulted != 0) {
		spin_unlock_irqrestore(&node->queue_lock, enabled);
		return EIO;
	}

	*available = runtime_free_markers(node);

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Succeeded: the count is a snapshot, not a promise. */
	return 0;
}

/*
 * Begins the stop of a session (its open is closing, or the core gave up
 * on it): the session takes no new frame, command or reservation from now
 * on.  What it queued still runs to its end; nothing here waits.
 */
static int
runtime_stop_begin(
	void *private_data,
	void *session,
	int error)
{
	struct nvrtx_node *node;
	struct nvrtx_session *owner;
	unsigned long enabled;

	UNUSED_PARAMETER(error);

	node = private_data;
	owner = session;

	/* stopping refuses the session's new work (submit, command, reserve answer EIO). */
	enabled = spin_lock_irqsave(&node->queue_lock);

	owner->stopping = 1;

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Succeeded: the stop is begun. */
	return 0;
}

/*
 * Reports whether a stopping session's work has ended: 0 once nothing it
 * queued is queued or running any more, on the GPU too (reservations it
 * never committed may remain: the core withdraws them), EAGAIN until then.
 * A run that does not end counts as running until the reaper stopped its
 * channel.
 */
static int
runtime_stop_poll(
	void *private_data,
	void *session)
{
	struct nvrtx_node *node;
	struct nvrtx_session *owner;
	unsigned long enabled;
	uint32_t reserved;
	uint32_t slot;
	uint32_t pending;
	bool stopped;

	node = private_data;
	owner = session;

	/* The session's pending work less its reservations not yet committed. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	reserved = 0U;
	for (slot = 0U; slot < NVRTX_NODE_MARKERS; slot++) {
		if (node->markers[slot].session == owner && node->markers[slot].state == NVRTX_ENTRY_RESERVED)
			reserved++;
	}
	pending = owner->pending - reserved;

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Work of the session still queued or running. */
	if (pending != 0U)
		return EAGAIN;

	/* Its runs on the GPU: ended, or their channel stopped (a lost run is stopped by the reaper). */
	stopped = runtime_session_stopped(node, owner);
	if (!stopped)
		return EAGAIN;

	/* Succeeded: the session's work has ended. */
	return 0;
}

/*
 * Stops new work after a fault: frames and markers queued afterwards are
 * refused, and the queued ones complete with EIO.  Idempotent.
 */
static void
runtime_fault(
	void *private_data,
	int error)
{
	struct nvrtx_node *node;
	unsigned long enabled;
	bool first;

	node = private_data;

	/* Sets the flag the queue's users look at; the first fault is shown. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	first = false;
	if (node->faulted == 0)
		first = true;
	node->faulted = 1;
	runtime_wake_lanes(node);

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	if (first)
		nvrtx_stage_mark("renderer: the renderer faulted (%d); it takes no more work", error);
}

/*
 * Isolates a session whose stop could not be confirmed in time: its
 * channel and memory stay the device's from now on (its close gives
 * nothing back and touches no hardware), its work answers EIO.  Returns 0
 * when the GPU no longer runs its work (its channel was stopped or freed,
 * it has none, or its runs have ended), else EIO: the device faults.  It
 * stops nothing itself (the reaper alone does).
 */
static int
runtime_isolate(
	void *private_data,
	void *session)
{
	struct nvrtx_node *node;
	struct nvrtx_session *owner;
	unsigned long enabled;
	bool stopped;

	node = private_data;
	owner = session;

	/* The session keeps everything and takes nothing new. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	owner->quarantined = 1;
	owner->stopping = 1;
	__atomic_store_n(&owner->run_lost, 1, __ATOMIC_RELEASE);

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Whether the GPU still runs its work. */
	stopped = runtime_session_stopped(node, owner);
	if (!stopped) {
		nvrtx_stage_mark("session isolation failed: its work may still run on the GPU");
		return EIO;
	}

	/* Succeeded: the session is isolated. */
	nvrtx_stage_mark("session isolated: its channel and memory are kept");
	return 0;
}

/*
 * Runs the queue in order for the node's life: a frame through the
 * executor, then its completion; a marker's completion, or its parking
 * when its session's newest run has not ended; then the entry is retired
 * and the session's pending count falls.
 */
static void
runtime_worker(
	void *argument)
{
	struct nvrtx_lane *lane;
	struct nvrtx_node *node;
	struct nvrtx_node_entry *entry;
	struct nvrtx_session *owner;
	unsigned long enabled;
	uint64_t observed;
	bool parked;
	int faulted;
	int error;

	lane = argument;
	node = lane->node;

	/* Takes the lane's entries one at a time, sleeping while its queue is empty. */
	for (;;) {
		enabled = spin_lock_irqsave(&node->queue_lock);

		while (lane->head == NULL) {
			observed = waitq_sequence(&lane->available);
			(void)waitq_sleep(&lane->available, &node->queue_lock, observed, 0U, 0U);
		}

		entry = lane->head;
		lane->head = entry->next;
		if (lane->head == NULL)
			lane->tail = NULL;
		entry->next = NULL;
		entry->state = NVRTX_ENTRY_RUNNING;
		owner = entry->session;
		faulted = node->faulted;

		spin_unlock_irqrestore(&node->queue_lock, enabled);

		/* A frame runs through the executor, unless the device faulted; a marker carries its error. */
		error = entry->error;
		if (entry->kind == RUNTIME_ENTRY_FRAME) {
			error = EIO;
			if (faulted == 0) {
				error = 0;
				if (entry->bytes != 0U)
					error = runtime_execute(node, entry->session, entry->wire, entry->bytes);
			}
		} else if (faulted != 0 && error == 0) {
			error = EIO;
		}

		/*
		 * A marker whose session's newest run has not ended goes to the
		 * completer.  The entry leaves the worker's queue before its
		 * completion (it still counts as pending), a marker once it has
		 * taken its run (runtime_park): the command its waiter sends
		 * next may run at once, and its runs are not the ones the marker
		 * waits for.
		 */
		if (entry->kind == RUNTIME_ENTRY_MARKER && error == 0) {
			parked = runtime_park(node, entry);
			if (parked)
				continue;

			/* The run ended: whether the session's runs are lost. */
			error = nvrtx_vk_node_run_wait(owner, entry->run_fence, entry->run_us);
		} else {
			runtime_dequeued(node, owner);
		}

		/* Delivers the completion, then retires the entry. */
		drv_gpu_complete(entry->completion, error);
		runtime_retire(node, entry);
	}
}

/* Takes an entry the worker ran (a frame, or a marker carrying an error) off its session's count of queued entries. */
static void
runtime_dequeued(
	struct nvrtx_node *node,
	struct nvrtx_session *owner)
{
	unsigned long enabled;

	enabled = spin_lock_irqsave(&node->queue_lock);

	owner->queued--;

	spin_unlock_irqrestore(&node->queue_lock, enabled);
}

/*
 * Completes the parked markers in the order they were parked, for the
 * node's life: each once its session's run has ended, with EIO when it did
 * not end in time or the device faulted.  The waits take no lock.
 */
static void
runtime_completer(
	void *argument)
{
	struct nvrtx_lane *lane;
	struct nvrtx_node *node;
	struct nvrtx_node_entry *entry;
	unsigned long enabled;
	uint64_t observed;
	int faulted;
	int error;

	lane = argument;
	node = lane->node;

	/* Takes the lane's oldest parked marker, sleeping while none is parked. */
	for (;;) {
		enabled = spin_lock_irqsave(&node->queue_lock);

		while (lane->parked_head == NULL) {
			observed = waitq_sequence(&lane->parked_more);
			(void)waitq_sleep(&lane->parked_more, &node->queue_lock, observed, 0U, 0U);
		}

		/* The head stays parked (counted pending) until it is completed. */
		entry = lane->parked_head;
		faulted = node->faulted;

		spin_unlock_irqrestore(&node->queue_lock, enabled);

		/* Waits for its run, unless the device faulted. */
		error = EIO;
		if (faulted == 0)
			error = nvrtx_vk_node_run_wait(entry->session, entry->run_fence, entry->run_us);

		/* Delivers the completion. */
		drv_gpu_complete(entry->completion, error);

		/* Takes it off the parked list, then retires it. */
		enabled = spin_lock_irqsave(&node->queue_lock);

		lane->parked_head = entry->next;
		if (lane->parked_head == NULL)
			lane->parked_tail = NULL;
		entry->next = NULL;

		spin_unlock_irqrestore(&node->queue_lock, enabled);

		runtime_retire(node, entry);
	}
}

/*
 * Parks a marker the worker took when its session's newest run has not
 * ended, or when markers are parked already (markers complete in order).
 * Returns true when parked: the completer completes it.
 */
static bool
runtime_park(
	struct nvrtx_node *node,
	struct nvrtx_node_entry *entry)
{
	struct nvrtx_lane *lane;
	unsigned long enabled;
	bool passed;

	/* The lane of the marker's session. */
	lane = runtime_lane(node, entry->session);

	/* The session's newest run, as its frames before the marker left it. */
	mutex_lock(&node->lock);

	entry->run_fence = entry->session->run_fence;
	entry->run_us = entry->session->run_us;

	mutex_unlock(&node->lock);

	/* Whether it still runs. */
	passed = nvrtx_vk_node_run_passed(entry->session, entry->run_fence);

	/*
	 * It leaves the worker's queue, with its run taken (before the
	 * completer may retire it); then it parks behind the parked ones when
	 * it must wait, or when others wait already.
	 */
	enabled = spin_lock_irqsave(&node->queue_lock);

	entry->session->queued--;
	if (passed && lane->parked_head == NULL) {
		spin_unlock_irqrestore(&node->queue_lock, enabled);
		return false;
	}
	entry->state = NVRTX_ENTRY_PARKED;
	entry->next = NULL;
	if (lane->parked_tail == NULL) {
		lane->parked_head = entry;
	} else {
		lane->parked_tail->next = entry;
	}

	lane->parked_tail = entry;
	waitq_wake_one(&lane->parked_more);

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Succeeded: the completer has it (counted for nvrtx.vk.perf=1). */
	nvrtx_node_count(node, &node->counts.parked, 1U);
	return true;
}

/*
 * Retires an entry whose completion was delivered: the session's pending
 * count falls, a frame is freed and a marker is free again (capacity).
 */
static void
runtime_retire(
	struct nvrtx_node *node,
	struct nvrtx_node_entry *entry)
{
	unsigned long enabled;
	uint32_t kind;

	/* The session's count, and a marker free again. */
	kind = entry->kind;
	enabled = spin_lock_irqsave(&node->queue_lock);

	entry->session->pending--;
	if (kind == RUNTIME_ENTRY_MARKER) {
		entry->state = NVRTX_ENTRY_FREE;
		entry->session = NULL;
		entry->completion = NULL;
	}

	/* The drains look again. */
	waitq_wake_all(&node->retired);

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* A frame's copy goes; a marker's freeing is capacity. */
	if (kind == RUNTIME_ENTRY_FRAME) {
		kern_free(entry);
	} else {
		drv_gpu_capacity_changed(node->device);
	}
}

/*
 * Looks at the channels and stops the one that holds the graphics engine
 * up, every period or when woken, for the node's life (after the stop for
 * the reboot it stops nothing).
 */
static void
runtime_reaper(
	void *argument)
{
	struct nvrtx_node *node;
	unsigned long enabled;
	uint64_t observed;
	uint64_t deadline;
	int stopping;

	node = argument;

	/* A period's sleep (or a wake), then a look. */
	for (;;) {
		enabled = spin_lock_irqsave(&node->queue_lock);

		observed = waitq_sequence(&node->reaper_wake);
		deadline = sched_ticks() + kern_ms_to_ticks(RUNTIME_REAPER_PERIOD_MS);
		(void)waitq_sleep(&node->reaper_wake, &node->queue_lock, observed, deadline, 0U);
		stopping = node->reaper_stop;

		spin_unlock_irqrestore(&node->queue_lock, enabled);

		/* The stop for the reboot owns the GSP now. */
		if (stopping != 0)
			continue;

		/* Looks at the channels once. */
		runtime_reap(node);
	}
}

/*
 * One look of the reaper: a GPU off the bus faults the device; else the
 * channel lost by an error report, or the one that holds the graphics
 * engine up, is stopped (when the boot allows it), under the lock of the
 * channels' making.
 */
static void
runtime_reap(
	struct nvrtx_node *node)
{
	struct nvrtx_work_channel *picked;
	unsigned long enabled;
	int bus_lost;
	int stopping;
	int found;

	/* The GPU off the bus: the whole device is lost. */
	bus_lost = __atomic_exchange_n(&node->bus_lost, 0, __ATOMIC_ACQ_REL);
	if (bus_lost != 0) {
		runtime_fault(node, EIO);
		drv_gpu_report_error(node->device, EIO);
		return;
	}

	/* The channel to stop, if any, with the channels held still. */
	mutex_lock(&node->channel_lock);

	found = runtime_pick(node, &picked);

	/* The stop, unless the boot asked for none or the reboot's stop began. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	stopping = node->reaper_stop;

	/* Marks the reaper busy, so the stop for the reboot waits for the stop that follows. */
	if (found != 0 &&
	    stopping == 0 &&
	    node->kills_wanted != 0)
		__atomic_store_n(&node->reaper_busy, 1, __ATOMIC_RELEASE);

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Stops the picked channel; when stops are not allowed, remembers that nothing else is picked. */
	if (found != 0 &&
	    stopping == 0 &&
	    node->kills_wanted != 0) {
		runtime_kill(node, picked);
		__atomic_store_n(&node->reaper_busy, 0, __ATOMIC_RELEASE);
	} else if (found != 0) {
		/* Not stopped (the boot asked for none): nothing else is picked, the other runs' waits end at their limit. */
		enabled = spin_lock_irqsave(&node->queue_lock);

		node->recovery_stuck = 1;

		spin_unlock_irqrestore(&node->queue_lock, enabled);
	}

	mutex_unlock(&node->channel_lock);
}

/*
 * Picks the channel to stop, the caller holding the lock of the channels'
 * making: one lost already (an error report, or picked before while stops
 * were not allowed it is not picked again), else the one that holds the
 * graphics engine up, which is lost now (its session, or every session
 * sharing the work channel, answers EIO from now on).  Nothing is picked
 * once a stop left a channel stuck.  Returns whether one was picked.
 */
static int
runtime_pick(
	struct nvrtx_node *node,
	struct nvrtx_work_channel **picked)
{
	struct nvrtx_recover_view views[RUNTIME_VIEWS];
	struct nvrtx_work_channel *channels[RUNTIME_VIEWS];
	struct nvrtx_session *owners[RUNTIME_VIEWS];
	struct nvrtx_work_channel *channel;
	unsigned long enabled;
	uint64_t kill_end;
	uint64_t now;
	uint32_t index;
	int culprit;

	/* Nothing is picked yet. */
	*picked = NULL;

	/* The sessions' channels and the shared one, each with its state; one lost already is stopped first. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	for (index = 0U; index < RUNTIME_VIEWS; index++) {
		owners[index] = NULL;
		channels[index] = NULL;
		if (index < NVRTX_WORK_SESSION_CHANNELS && node->channels[index] != NULL) {
			owners[index] = node->channels[index];
			channels[index] = owners[index]->channel;
		} else if (index == NVRTX_WORK_SESSION_CHANNELS) {
			channels[index] = &node->gpu->work.driver;
		}

		/* A ready channel may be stopped, unless a stop left one stuck. */
		views[index].stoppable = 0;
		if (channels[index] != NULL &&
		    channels[index]->state == NVRTX_WORK_CHANNEL_READY &&
		    node->recovery_stuck == 0)
			views[index].stoppable = 1;

		/* The first channel already lost is the one to stop, when stops are allowed. */
		if (channels[index] != NULL &&
		    channels[index]->state == NVRTX_WORK_CHANNEL_LOST &&
		    node->kills_wanted != 0 &&
		    *picked == NULL)
			*picked = channels[index];
	}

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* A channel lost already is stopped before another is looked for. */
	if (*picked != NULL)
		return 1;

	/* What each channel's GPU side shows. */
	for (index = 0U; index < RUNTIME_VIEWS; index++) {
		views[index].busy = 0;
		views[index].resident = 0;
		if (channels[index] != NULL)
			nvrtx_work_channel_look(channels[index], &views[index]);
	}

	/* Whether any run waited its time; then the engine is asked which channel it holds. */
	now = nvrtx_now_us();
	kill_end = __atomic_load_n(&node->kill_end_us, __ATOMIC_ACQUIRE);
	culprit = nvrtx_recover_culprit(views, RUNTIME_VIEWS, now, kill_end, NVRTX_NODE_RUN_US);

	/* No run has waited its time: nothing is to be stopped. */
	if (culprit < 0)
		return 0;

	/* Asks the engine which busy channel it holds. */
	for (index = 0U; index < RUNTIME_VIEWS; index++) {
		if (views[index].busy != 0)
			views[index].resident = (int)nvrtx_work_channel_resident(node->gpu, channels[index]);
	}

	/* Picks again with the engine's answer. */
	culprit = nvrtx_recover_culprit(views, RUNTIME_VIEWS, now, kill_end, NVRTX_NODE_RUN_US);

	/* No channel can be stopped: nothing is picked. */
	if (culprit < 0)
		return 0;

	/* The channel to blame. */
	channel = channels[culprit];

	/* The culprit is lost, once: its session (or the work channel's sessions) answers EIO from now on. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	if (channel->state == NVRTX_WORK_CHANNEL_READY) {
		channel->state = NVRTX_WORK_CHANNEL_LOST;
		channel->lost_reason = ETIMEDOUT;
		if (owners[culprit] != NULL)
			__atomic_store_n(&owners[culprit]->run_lost, 1, __ATOMIC_RELEASE);
	}

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Says what the engine showed of the lost channel. */
	nvrtx_stage_mark("channel %u lost: its run held the graphics engine (resident %d, started %d)",
	    (unsigned)channel->chid,
	    views[culprit].resident,
	    views[culprit].started);

	/* Succeeded: the channel is picked. */
	*picked = channel;
	return 1;
}

/*
 * Stops a lost channel on the GPU, the caller holding the lock of the
 * channels' making: the stop at once; when it fails, the GSP's error report
 * on the channel is waited for a while; without either, the channel and its
 * engine are reset.  With any of them the channel is freed in the resource
 * manager.  The channel's state says what is left (freed, stopped or stuck);
 * the other runs' time counts from the end.
 */
static void
runtime_kill(
	struct nvrtx_node *node,
	struct nvrtx_work_channel *channel)
{
	unsigned long enabled;
	const char *outcome;
	uint64_t start;
	uint64_t now;
	uint64_t spent;
	uint32_t state;
	uint32_t rc_type;
	int stopped;
	int reported;
	int reset;
	int freed;
	int failures;
	int error;

	/* The stop's time is counted from here. */
	start = nvrtx_now_us();

	/* Being stopped. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	channel->state = NVRTX_WORK_CHANNEL_KILLING;

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* The stop at once. */
	error = nvrtx_work_channel_stop(node->gpu, channel);
	stopped = 0;
	if (error == 0)
		stopped = 1;

	/* Says how long the stop took (the resource manager waits for the preempt). */
	now = nvrtx_now_us();
	spent = now - start;
	nvrtx_stage_mark("channel %u: stop answered %d after %u ms", (unsigned)channel->chid, error, (unsigned)(spent / 1000U));

	/* A stop that failed: the GSP reports the channel's error once it has taken it off (the drain reads the report). */
	reported = 0;
	for (;;) {
		rc_type = __atomic_load_n(&channel->rc_type, __ATOMIC_ACQUIRE);
		if (rc_type != 0U)
			reported = 1;
		spent = nvrtx_now_us() - start;

		/* The stop answered, the report came, or the wait is spent. */
		if (stopped != 0 ||
		    reported != 0 ||
		    spent > RUNTIME_REPORT_WAIT_US)
			break;

		/* Gives the drain the time to read the report. */
		kern_usleep_range(RUNTIME_REPORT_STEP_US, RUNTIME_REPORT_STEP_US * 2U);
	}

	/* Neither: the channel and its engine are reset. */
	reset = 0;
	if (stopped == 0 && reported == 0) {
		error = nvrtx_work_channel_reset(node->gpu, channel, 0);
		if (error == 0)
			reset = 1;

		/* Says how long it took with the stop. */
		now = nvrtx_now_us();
		spent = now - start;
		nvrtx_stage_mark("channel %u: reset answered %d after %u ms", (unsigned)channel->chid, error, (unsigned)(spent / 1000U));
	}

	/* The resource manager frees a channel the GPU no longer runs. */
	freed = 0;
	if (stopped != 0 ||
	    reported != 0 ||
	    reset != 0) {
		failures = nvrtx_work_channel_free(node->gpu, channel);
		if (failures == 0)
			freed = 1;
	}

	/* What the stop left, and when it ended. */
	state = nvrtx_recover_kill_state(stopped, reported, reset, freed);

	/* Publishes the channel's state; a stuck channel stops every later pick. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	channel->state = state;
	if (state == NVRTX_WORK_CHANNEL_STUCK)
		node->recovery_stuck = 1;

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* The other runs' time counts from now. */
	now = nvrtx_now_us();
	__atomic_store_n(&node->kill_end_us, now, __ATOMIC_RELEASE);
	spent = nvrtx_now_us() - start;

	/* Says how it went. */
	if (state == NVRTX_WORK_CHANNEL_STUCK) {
		nvrtx_stage_mark("channel %u stuck: no stop, report or reset took it off (%u ms)", (unsigned)channel->chid, (unsigned)(spent / 1000U));
		return;
	}

	/* Says whether the resource manager freed the channel or keeps it. */
	outcome = ", kept";
	if (state == NVRTX_WORK_CHANNEL_FREED)
		outcome = " and freed";

	/* Succeeded: the GPU no longer runs the channel. */
	nvrtx_stage_mark("channel %u stopped%s (stop %d, report %d, reset %d, %u ms)",
	    (unsigned)channel->chid,
	    outcome,
	    stopped,
	    reported,
	    reset,
	    (unsigned)(spent / 1000U));
}

/*
 * Takes the GSP's error report on a channel (any thread reading the GSP's
 * messages under its lock; spinlocks only): a session's channel, or the
 * shared work channel, is lost (its session, or the work channel's
 * sessions, answer EIO) and the reaper stops it; a GPU off the bus faults
 * the device from the reaper.  Reports on the driver's other channels are
 * only shown (by the drain).
 */
static void
runtime_channel_error(
	void *argument,
	uint32_t chid,
	uint32_t type)
{
	struct nvrtx_node *node;
	struct nvrtx_session *owner;
	struct nvrtx_work_channel *channel;
	unsigned long enabled;

	node = argument;

	/* The GPU off the bus: the reaper faults the device. */
	if (type == RUNTIME_RC_OFF_THE_BUS)
		__atomic_store_n(&node->bus_lost, 1, __ATOMIC_RELEASE);

	/* The channel the report names, and its session. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	owner = NULL;
	channel = NULL;
	if (chid >= NVRTX_WORK_SESSION_CHID_FIRST && chid - NVRTX_WORK_SESSION_CHID_FIRST < NVRTX_WORK_SESSION_CHANNELS)
		owner = node->channels[chid - NVRTX_WORK_SESSION_CHID_FIRST];
	if (owner != NULL)
		channel = owner->channel;
	if (owner == NULL && chid == node->gpu->work.driver.chid)
		channel = &node->gpu->work.driver;

	/* The report is kept, and a ready channel is lost. */
	if (channel != NULL) {
		__atomic_store_n(&channel->rc_type, type + 1U, __ATOMIC_RELEASE);
		if (channel->state == NVRTX_WORK_CHANNEL_READY) {
			channel->state = NVRTX_WORK_CHANNEL_LOST;
			channel->lost_reason = EIO;
			if (owner != NULL)
				__atomic_store_n(&owner->run_lost, 1, __ATOMIC_RELEASE);
		}
	}

	/* The reaper looks at once. */
	waitq_wake_all(&node->reaper_wake);

	spin_unlock_irqrestore(&node->queue_lock, enabled);
}

/*
 * Reports whether a session's runs no longer run on the GPU: its channel
 * (or the shared work channel) was freed or stopped, it has none, or its
 * newest run has ended.
 */
static bool
runtime_session_stopped(
	struct nvrtx_node *node,
	struct nvrtx_session *session)
{
	struct nvrtx_work_channel *channel;
	unsigned long enabled;
	uint32_t state;
	uint32_t fence;
	bool passed;
	bool stopped;
	int busy;

	/* A session that never ran a frame has nothing on the GPU. */
	channel = __atomic_load_n(&session->channel, __ATOMIC_ACQUIRE);
	if (channel == NULL)
		return true;

	/* The channel's state. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	state = channel->state;

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* The session's newest run. */
	fence = __atomic_load_n(&session->run_fence, __ATOMIC_ACQUIRE);
	passed = nvrtx_vk_node_run_passed(session, fence);

	/* Succeeded: whether the GPU still runs the session's work. */
	busy = 1;
	if (passed)
		busy = 0;
	stopped = nvrtx_recover_stopped(state, busy);
	return stopped;
}

/*
 * Runs a frame of a session through the executor under the node's lock;
 * with nvrtx.vk.perf=1 the frame, its wait for the lock and its hold of it
 * are counted.  Returns the executor's error.
 */
static int
runtime_execute(
	struct nvrtx_node *node,
	struct nvrtx_session *session,
	const void *wire,
	uint32_t bytes)
{
	uint64_t asked;
	uint64_t taken;
	uint64_t left;
	int error;

	/* The session's channel, before its first frame takes the node's lock. */
	runtime_channel(node, session);

	/* Runs the frame holding the session's executor, timing the node's lock's wait and hold only when the counts are asked for. */
	asked = 0U;
	if (node->perf != 0)
		asked = nvrtx_now_us();

	mutex_lock(&session->exec_lock);
	mutex_lock(&node->lock);

	taken = 0U;
	if (node->perf != 0)
		taken = nvrtx_now_us();
	error = nvrtx_vk_stream_execute(session->vk, wire, bytes, nvrtx_vk_dispatch);

	mutex_unlock(&node->lock);
	mutex_unlock(&session->exec_lock);

	/* Counts the frame, its wait and its hold. */
	if (node->perf != 0) {
		left = nvrtx_now_us();
		nvrtx_node_count(node, &node->counts.frames, 1U);
		nvrtx_node_count(node, &node->counts.wait_us, taken - asked);
		nvrtx_node_count(node, &node->counts.held_us, left - taken);
	}

	/* Reports a frame the executor refused or that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the frame ran. */
	return 0;
}

/*
 * Gives a session its channel when it has none yet (before its first
 * frame or marker is queued or run): a channel of its own under the next
 * free number, with that number's lane, or the driver's work channel to
 * share (and the main lane) when the boot asked for none, every number is
 * in use or the making failed.  A faulted device or a stopping session
 * gets none.  The node's lock is not held (the making calls the GSP).
 */
static void
runtime_channel(
	struct nvrtx_node *node,
	struct nvrtx_session *session)
{
	struct nvrtx_work_channel *made;
	struct nvrtx_work_channel *given;
	unsigned long enabled;
	int refused;
	int number;
	int error;
	int lane_error;

	/* A session that has its channel keeps it (it is given once and never changes). */
	given = __atomic_load_n(&session->channel, __ATOMIC_ACQUIRE);
	if (given != NULL)
		return;

	/* Looks again and makes under the lock of the channels' making. */
	mutex_lock(&node->channel_lock);

	if (session->channel != NULL) {
		mutex_unlock(&node->channel_lock);
		return;
	}

	/* A faulted device, or a session being stopped, gets none (its work answers EIO). */
	enabled = spin_lock_irqsave(&node->queue_lock);

	refused = node->faulted;
	if (session->stopping != 0)
		refused = 1;

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Refused: the session goes without a channel. */
	if (refused != 0) {
		mutex_unlock(&node->channel_lock);
		return;
	}

	/* A free number of the sessions', unless every session shares the work channel. */
	number = -1;
	if (node->channels_wanted != 0)
		number = nvrtx_recover_chid_next(node->chid_used, node->chid_last, NVRTX_WORK_SESSION_CHANNELS);

	/* The channel under that number. */
	made = NULL;
	error = ENOSPC;
	if (number >= 0)
		made = kern_calloc(1U, sizeof(*made));
	if (made != NULL)
		error = nvrtx_work_channel_open(node->gpu, made, NVRTX_WORK_SESSION_CHID_FIRST + (uint32_t)number);
	if (error == 0) {
		node->chid_used |= 1U << (uint32_t)number;
		node->chid_last = (uint32_t)number;
	} else if (made != NULL &&
		   made->arena_made == 0 &&
		   made->region_made == 0 &&
		   made->channel_made == 0) {
		/* Nothing of a failed channel stays: its record goes. */
		kern_free(made);
		made = NULL;
	} else if (made != NULL) {
		/* What the resource manager kept of a failed channel stays the device's, with its number. */
		node->chid_used |= 1U << (uint32_t)number;
		node->chid_last = (uint32_t)number;
		made = NULL;
	}

	/* The session's channel: its own, or the shared one. */
	if (error == 0) {
		enabled = spin_lock_irqsave(&node->queue_lock);

		made->state = NVRTX_WORK_CHANNEL_READY;
		node->channels[number] = session;

		spin_unlock_irqrestore(&node->queue_lock, enabled);

		/* Its number's lane, when its threads run (else the main lane). */
		lane_error = runtime_lane_start(&node->lanes[number]);
		if (lane_error == 0)
			__atomic_store_n(&session->lane, &node->lanes[number], __ATOMIC_RELEASE);

		/* A lane without threads is told; the main lane serves the session. */
		if (lane_error != 0)
			nvrtx_stage_mark("renderer: channel %u's lane has no threads (%d); the main lane serves it", (unsigned)made->chid, lane_error);
		__atomic_store_n(&session->channel, made, __ATOMIC_RELEASE);
	} else {
		session->shared = 1;
		__atomic_store_n(&session->channel, &node->gpu->work.driver, __ATOMIC_RELEASE);
	}

	mutex_unlock(&node->channel_lock);

	/* A session that shares the work channel is told. */
	if (error != 0 && node->channels_wanted != 0)
		nvrtx_stage_mark("renderer: a session shares the work channel (%d)", error);
}

/*
 * Starts a lane's completer and worker the first time a session takes its
 * number; they serve the lane for the node's life.  Returns 0 once both
 * run (a lane started before needs nothing), else the thread's error: the
 * session then goes through the main lane.
 */
static int
runtime_lane_start(
	struct nvrtx_lane *lane)
{
	struct thread *completer;
	struct thread *worker;
	int error;

	/* A lane already served. */
	if (lane->started != 0)
		return 0;

	/* The completer of its parked markers, then its worker. */
	error = kthread_create(runtime_completer, lane, SCHED_PRIORITY_DEFAULT, &completer);
	if (error != 0)
		return error;
	completer->detached = 1;
	thread_start(completer);

	/* The lane's worker. */
	error = kthread_create(runtime_worker, lane, SCHED_PRIORITY_DEFAULT, &worker);
	if (error != 0)
		return error;
	worker->detached = 1;
	thread_start(worker);

	/* Succeeded: the lane takes entries. */
	lane->started = 1;
	return 0;
}

/* Gives the lane a session's frames and markers go through: its number's, or the main one. */
static struct nvrtx_lane *
runtime_lane(
	struct nvrtx_node *node,
	struct nvrtx_session *session)
{
	struct nvrtx_lane *lane;

	/* The session's own lane, set with its channel, else the main one. */
	lane = __atomic_load_n(&session->lane, __ATOMIC_ACQUIRE);
	if (lane == NULL)
		return &node->main_lane;

	/* Succeeded: the session's lane. */
	return lane;
}

/* Wakes every lane's worker (a fault or the stop: what is queued completes with EIO); the caller holds queue_lock. */
static void
runtime_wake_lanes(
	struct nvrtx_node *node)
{
	uint32_t index;

	/* The main lane, then each session's number's. */
	waitq_wake_all(&node->main_lane.available);
	for (index = 0U; index < NVRTX_WORK_SESSION_CHANNELS; index++)
		waitq_wake_all(&node->lanes[index].available);
}

/* Appends an entry to its session's lane and wakes the lane's worker; the caller holds queue_lock. */
static void
runtime_enqueue(
	struct nvrtx_node *node,
	struct nvrtx_node_entry *entry)
{
	struct nvrtx_lane *lane;

	/* At the tail of its session's lane, counted in its session's queue. */
	entry->state = NVRTX_ENTRY_QUEUED;
	entry->session->queued++;
	entry->next = NULL;
	lane = runtime_lane(node, entry->session);
	if (lane->tail == NULL) {
		lane->head = entry;
	} else {
		lane->tail->next = entry;
	}

	lane->tail = entry;

	/* The worker takes it. */
	waitq_wake_one(&lane->available);
}

/* Finds the marker a token names for the session and completion, or NULL; the caller holds queue_lock. */
static struct nvrtx_node_entry *
runtime_marker(
	struct nvrtx_node *node,
	struct nvrtx_session *session,
	void *token,
	struct drv_gpu_completion *completion)
{
	struct nvrtx_node_entry *entry;
	uint32_t slot;

	/* Compares the token with each marker before using it. */
	for (slot = 0U; slot < NVRTX_NODE_MARKERS; slot++) {
		entry = &node->markers[slot];
		if (token != (void *)entry)
			continue;
		if (entry->session != session || entry->completion != completion)
			return NULL;
		if (entry->state == NVRTX_ENTRY_FREE)
			return NULL;

		/* The token names this marker of the session's. */
		return entry;
	}

	/* No marker is named. */
	return NULL;
}

/* Counts the free markers; the caller holds queue_lock. */
static unsigned
runtime_free_markers(
	const struct nvrtx_node *node)
{
	unsigned count;
	uint32_t slot;

	/* Each free one. */
	count = 0U;
	for (slot = 0U; slot < NVRTX_NODE_MARKERS; slot++) {
		if (node->markers[slot].state == NVRTX_ENTRY_FREE)
			count++;
	}

	/* Succeeded: the count. */
	return count;
}

/* Reports whether a queue of the session's executor owns the timeline. */
static bool
runtime_timeline_owned(
	struct nvrtx_session *session,
	uint32_t timeline)
{
	uint64_t timelines;

	/* The executor records the timelines its queues were made with. */
	timelines = __atomic_load_n(&session->vk->timelines, __ATOMIC_ACQUIRE);
	if ((timelines & (1ULL << timeline)) == 0U)
		return false;

	/* Succeeded: the timeline is the session's. */
	return true;
}
