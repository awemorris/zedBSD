/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU device's command transport and recovery.
 *
 * With an executor bound, the node is a renderer libvulkan opens: it
 * reports the protocol's capset, runs command frames (synchronously, or
 * queued with a completion), and takes supervised jobs whose completion
 * markers follow the frames queued before them.  One worker thread runs
 * the queue in order: a frame through the executor under the node's lock,
 * a marker by completing it.  The executor's submissions do not wait for
 * the GPU, so a marker whose session's runs the GPU has not ended yet is
 * parked, and the completer thread completes the parked markers in order
 * as their runs end (a run that does not end completes its marker with
 * EIO).  A fault stops new work: frames and markers still queued complete
 * with EIO, and no address space runs anything new.
 * The stop of a session and the device's fault are there whether an
 * executor is bound or not.
 *
 * queue_lock (a spinlock) guards the queue, the markers, every session's
 * pending count and stopping, and the fault flag; the node's lock (a
 * mutex) is taken only outside it, around the executor.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>
#include <uapi/gpu-job.h>

#include "drivers/gpu/amdrdna/amdrdna-node.h"
#include "drivers/gpu/amdrdna/amdrdna-reset.h"

/*
 * The protocol's capset: its id, its bytes and the places of its words,
 * the wire version, the XML version (1.3.269), the vendor suffix
 * ("SDBZ") with its flags, and the native word ("TANZ") with its flags,
 * as libvulkan reads them.
 *
 * Values referenced: zedBSD userland/desktop/libvulkan/context.c (the
 * capset the transport asks for and the words it reads).
 */
#define RUNTIME_CAPSET_ID		4U
#define RUNTIME_CAPSET_BYTES		176U
#define RUNTIME_CAPSET_WIRE_AT		0U
#define RUNTIME_CAPSET_XML_AT		4U
#define RUNTIME_CAPSET_TIMELINES_AT	152U
#define RUNTIME_CAPSET_MAGIC_AT		160U
#define RUNTIME_CAPSET_FLAGS_AT		164U
#define RUNTIME_CAPSET_NATIVE_AT	168U
#define RUNTIME_CAPSET_NATIVE_FLAGS_AT	172U
#define RUNTIME_WIRE_VERSION		1U
#define RUNTIME_XML_VERSION		((1U << 22) | (3U << 12) | 269U)
#define RUNTIME_VENDOR_MAGIC		0x5a424453U

/* The flags: opaque sharing, strict queues, checked quiescence (the profile libvulkan pairs with). */
#define RUNTIME_VENDOR_FLAGS		7U

/*
 * The native word: its magic, and the flag of a command run at once on
 * the caller's thread (GPU_COMMAND), its replies written when the call
 * returns.
 */
#define RUNTIME_NATIVE_MAGIC		0x5a4e4154U
#define RUNTIME_NATIVE_SYNC_COMMAND	2U

/* The bits of a 32-bit word stored byte by byte, lowest first. */
#define RUNTIME_BYTE_BITS		8U
#define RUNTIME_WORD_BYTES		4U

/* The timelines a queue may name (1 to 63; 0 is the context's own). */
#define RUNTIME_TIMELINES		64U

/* How long a parked marker's run may take before its space is lost and its queue reset. */
#define RUNTIME_RUN_MS			2000U

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
static int runtime_stop_begin(void *private_data, void *session, int error);
static int runtime_stop_poll(void *private_data, void *session);
static int runtime_isolate(void *private_data, void *session);
static void runtime_fault(void *private_data, int error);
static int runtime_reset(void *private_data);
static void runtime_worker(void *argument);
static void runtime_completer(void *argument);
static bool runtime_park(struct amdrdna_node *node, struct amdrdna_node_entry *entry);
static void runtime_dequeued(struct amdrdna_node *node, struct amdrdna_session *owner);
static int runtime_runs_wait(struct amdrdna_session *session, const struct amdrdna_space_ticket *runs, unsigned milliseconds);
static void runtime_retire(struct amdrdna_node *node, struct amdrdna_node_entry *entry);
static int runtime_execute(struct amdrdna_node *node, struct amdrdna_session *session, const void *wire, uint32_t bytes);
static void runtime_enqueue(struct amdrdna_node *node, struct amdrdna_node_entry *entry);
static struct amdrdna_node_entry *runtime_marker(struct amdrdna_node *node, struct amdrdna_session *session, void *token, struct drv_gpu_completion *completion);
static unsigned runtime_free_markers(const struct amdrdna_node *node);
static bool runtime_timeline_owned(struct amdrdna_node *node, struct amdrdna_session *session, uint32_t timeline);
static void runtime_put32(uint8_t *bytes, uint32_t at, uint32_t value);

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
	NULL
};

/*
 * The recovery operations: a session's stop and its poll, the device's
 * fault, and the isolation of a session whose run was given up and whose
 * queue was reset; the device is not reset.
 */
static const struct drv_gpu_recovery_ops runtime_recovery = {
	runtime_stop_begin,
	runtime_stop_poll,
	runtime_fault,
	NULL,
	runtime_isolate
};

/*
 * The same with the device's reset (a mode-1 reset and the stages set up
 * again), for a boot that allows it (amdrdna.reset=1).
 */
static const struct drv_gpu_recovery_ops runtime_recovery_reset = {
	runtime_stop_begin,
	runtime_stop_poll,
	runtime_fault,
	runtime_reset,
	runtime_isolate
};

/*
 * Prepares the renderer's queue, its lock and wait queues, and the
 * recovery operations before the node is registered.
 */
void
amdrdna_runtime_prepare(
	struct amdrdna_node *node)
{
	bool allowed;

	/* The queue, its lock and its two wait queues. */
	spin_init(&node->queue_lock, LOCK_RANK_DEVICE, "amdrdna queue");
	waitq_init(&node->available, "amdrdna queue available");
	waitq_init(&node->retired, "amdrdna queue retired");
	waitq_init(&node->parked_more, "amdrdna queue parked");
	node->parked_head = NULL;
	node->parked_tail = NULL;

	/* The stop of a session and the device's fault, and its reset when the boot allows it. */
	allowed = amdrdna_reset_allowed(node->device);
	node->ops.recovery = &runtime_recovery;
	if (allowed)
		node->ops.recovery = &runtime_recovery_reset;
}

/*
 * Binds an executor to the node before it is registered: its frame runner
 * and the queue of timelines it records, the renderer's operations and
 * the capabilities they make good.
 */
void
amdrdna_runtime_bind(
	struct amdrdna_node *node,
	amdrdna_node_execute execute,
	amdrdna_node_timelines timelines)
{
	/* The executor's two entries. */
	node->execute = execute;
	node->timelines = timelines;

	/* The operations and the capabilities they make good. */
	node->ops.get_capset = runtime_capset;
	node->ops.command = runtime_command;
	node->ops.commands = &runtime_commands;
	node->ops.jobs = &runtime_jobs;
	node->ops.capabilities |= GPU_CAP_CAPSET | GPU_CAP_COMMAND | GPU_CAP_NOTIFICATION | GPU_CAP_JOB | GPU_CAP_JOB_CAPACITY;
	node->renderer = 1;
}

/*
 * Starts the worker once the node is registered; it runs for the node's
 * life.  Without it nothing queued would complete, so a failure faults the
 * renderer (every queued operation then answers EIO).
 */
void
amdrdna_runtime_start(
	struct amdrdna_node *node)
{
	struct thread *worker;
	struct thread *completer;
	unsigned long enabled;
	int error;

	/* The worker, then the completer of the parked markers. */
	error = kthread_create(runtime_worker, node, SCHED_PRIORITY_DEFAULT, &worker);
	if (error == 0) {
		worker->detached = 1;
		thread_start(worker);
		error = kthread_create(runtime_completer, node, SCHED_PRIORITY_DEFAULT, &completer);
	}
	if (error == 0) {
		completer->detached = 1;
		thread_start(completer);
		return;
	}

	/* No worker or no completer: the renderer takes nothing. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	node->faulted = 1;

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Says why the renderer takes nothing. */
	amdrdna_mark("gpu device: no renderer worker or completer (%d)", error);
}

/*
 * Stops the renderer for the reboot: nothing new is taken and what is
 * queued completes with EIO, as after a fault, but without a fault's
 * report.  The worker thread, when there is one, runs on, idle, until
 * the reset.
 */
void
amdrdna_runtime_stop(
	struct amdrdna_node *node)
{
	unsigned long enabled;

	/* faulted refuses every later frame, marker and reservation; the worker completes the rest with EIO. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	node->faulted = 1;
	waitq_wake_all(&node->available);

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Marks the stop, for the log of the reboot. */
	amdrdna_mark("shutdown: the renderer takes no more work");
}

/*
 * Holds the renderer across a reset of the GPU (held true: nothing new is
 * taken and what is queued completes with EIO, as after a fault but
 * without a fault's report), or lets it take work again after the reset
 * set the GPU up (held false).
 */
void
amdrdna_runtime_hold(
	struct amdrdna_node *node,
	bool held)
{
	unsigned long enabled;

	/* faulted is what every frame, marker and reservation looks at. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	node->faulted = 0;
	if (held)
		node->faulted = 1;
	waitq_wake_all(&node->available);

	spin_unlock_irqrestore(&node->queue_lock, enabled);
}

/* Reports the protocol's capset: wire 1, XML 1.3.269, timelines, the vendor suffix and its flags. */
static int
runtime_capset(
	void *private_data,
	void *session,
	struct gpu_capset *capset)
{
	UNUSED_PARAMETER(private_data);
	UNUSED_PARAMETER(session);

	/* Only the protocol's capset, version 0, in a reply that holds it. */
	if (capset->capset_id != RUNTIME_CAPSET_ID || capset->capset_version != 0U)
		return ENOTSUP;
	if (capset->capacity < RUNTIME_CAPSET_BYTES || capset->capacity > GPU_CAPSET_MAX)
		return EINVAL;

	/* Every word not named is zero. */
	kern_memset(capset->data, 0, sizeof(capset->data));
	runtime_put32(capset->data, RUNTIME_CAPSET_WIRE_AT, RUNTIME_WIRE_VERSION);
	runtime_put32(capset->data, RUNTIME_CAPSET_XML_AT, RUNTIME_XML_VERSION);
	runtime_put32(capset->data, RUNTIME_CAPSET_TIMELINES_AT, 1U);
	runtime_put32(capset->data, RUNTIME_CAPSET_MAGIC_AT, RUNTIME_VENDOR_MAGIC);
	runtime_put32(capset->data, RUNTIME_CAPSET_FLAGS_AT, RUNTIME_VENDOR_FLAGS);
	runtime_put32(capset->data, RUNTIME_CAPSET_NATIVE_AT, RUNTIME_NATIVE_MAGIC);
	runtime_put32(capset->data, RUNTIME_CAPSET_NATIVE_FLAGS_AT, RUNTIME_NATIVE_SYNC_COMMAND);
	capset->bytes = RUNTIME_CAPSET_BYTES;

	/* Succeeded: the capset is reported. */
	return 0;
}

/*
 * Runs one frame at once on the caller's thread; EAGAIN while frames or
 * markers of the session are in the worker's queue or running there (a
 * frame may not overtake them).  The frame counts as the session's
 * pending work while it runs, so a stop or a drain waits for it; its
 * waits for the GPU are bounded as the worker's are.
 */
static int
runtime_command(
	void *private_data,
	void *session,
	const void *wire,
	uint32_t bytes)
{
	struct amdrdna_node *node;
	struct amdrdna_session *owner;
	unsigned long enabled;
	uint32_t queued;
	int faulted;
	int error;

	/* The node and what the core passed in. */
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

	/* A frame that failed may have run in part: as a queued frame's failure, the session's stream is lost. */
	if (error != 0)
		return EIO;

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
	struct amdrdna_node *node;
	struct amdrdna_node_entry *entry;
	unsigned long enabled;
	int faulted;

	/* The node and what the core passed in. */
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

	/* Allocates the entry with its copy of the frame. */
	entry = kern_calloc(1U, sizeof(*entry) + bytes);
	if (entry == NULL)
		return ENOMEM;

	/* Describes the frame: its session, its completion and its copy. */
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
	struct amdrdna_node *node;
	struct amdrdna_session *owner;
	unsigned long enabled;
	uint64_t observed;

	/* The node and what the core passed in. */
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
	struct amdrdna_node *node;
	struct amdrdna_session *owner;
	struct amdrdna_node_entry *entry;
	unsigned long enabled;
	uint32_t slot;
	bool owned;

	/* The node and what the core passed in. */
	node = private_data;
	owner = session;
	*token = NULL;

	/* A timeline of the session's queues. */
	if (timeline == 0U || timeline >= RUNTIME_TIMELINES)
		return EINVAL;
	owned = runtime_timeline_owned(node, owner, timeline);
	if (!owned)
		return EINVAL;

	/* Takes a free marker unless the device faulted or the session is being stopped. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	if (node->faulted != 0 || owner->stopping != 0) {
		spin_unlock_irqrestore(&node->queue_lock, enabled);
		return EIO;
	}

	/* Finds a free marker. */
	entry = NULL;
	for (slot = 0U; slot < AMDRDNA_NODE_MARKERS; slot++) {
		if (node->markers[slot].state == AMDRDNA_ENTRY_FREE) {
			entry = &node->markers[slot];
			break;
		}
	}

	/* No marker is free: the producer waits for capacity. */
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
	entry->state = AMDRDNA_ENTRY_RESERVED;
	owner->pending++;

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Succeeded: the token names the marker. */
	*token = entry;
	return 0;
}

/* Queues a reserved marker after the frames queued before it. */
static int
runtime_commit(
	void *private_data,
	void *session,
	void *token,
	struct drv_gpu_completion *completion)
{
	struct amdrdna_node *node;
	struct amdrdna_node_entry *entry;
	unsigned long enabled;

	/* The node and what the core passed in. */
	node = private_data;

	/* Finds the reservation, queues it unless the device faulted. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	entry = runtime_marker(node, session, token, completion);
	if (entry == NULL || entry->state != AMDRDNA_ENTRY_RESERVED) {
		spin_unlock_irqrestore(&node->queue_lock, enabled);
		return ESTALE;
	}

	/* A faulted device queues nothing. */
	if (node->faulted != 0) {
		spin_unlock_irqrestore(&node->queue_lock, enabled);
		return EIO;
	}

	/* Queues the marker after the frames before it. */
	runtime_enqueue(node, entry);

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Succeeded: the worker completes the marker in order. */
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
	struct amdrdna_node *node;
	struct amdrdna_node_entry *entry;
	unsigned long enabled;

	/* The node and what the core passed in. */
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
		if (entry->state == AMDRDNA_ENTRY_RESERVED) {
			entry->error = EIO;
			runtime_enqueue(node, entry);
		}

		spin_unlock_irqrestore(&node->queue_lock, enabled);
		return 0;
	}

	/* A normal cancel withdraws only a reservation not yet queued. */
	if (entry->state != AMDRDNA_ENTRY_RESERVED) {
		spin_unlock_irqrestore(&node->queue_lock, enabled);
		return ESTALE;
	}

	/* The marker is free again; the session no longer waits for it. */
	entry->state = AMDRDNA_ENTRY_FREE;
	entry->session->pending--;
	entry->session = NULL;
	entry->completion = NULL;
	waitq_wake_all(&node->retired);

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Succeeded: the reservation is withdrawn without a completion (the GPU core tells capacity observers). */
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
	struct amdrdna_node *node;
	unsigned long enabled;
	bool owned;

	/* The node and what the core passed in. */
	node = private_data;
	*available = 0U;

	/* A timeline of the session's queues. */
	if (timeline == 0U || timeline >= RUNTIME_TIMELINES)
		return EINVAL;
	owned = runtime_timeline_owned(node, session, timeline);
	if (!owned)
		return EINVAL;

	/* The free markers, unless the device faulted. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	if (node->faulted != 0) {
		spin_unlock_irqrestore(&node->queue_lock, enabled);
		return EIO;
	}

	/* The free markers now. */
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
	struct amdrdna_node *node;
	struct amdrdna_session *owner;
	unsigned long enabled;

	UNUSED_PARAMETER(error);

	/* The node and what the core passed in. */
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
 * queued is queued or running any more (reservations it never committed
 * may remain: the core withdraws them) and its address space runs nothing
 * on the GPU, EAGAIN until then.
 */
static int
runtime_stop_poll(
	void *private_data,
	void *session)
{
	struct amdrdna_node *node;
	struct amdrdna_session *owner;
	unsigned long enabled;
	uint32_t reserved;
	uint32_t slot;
	uint32_t pending;
	bool quiet;

	/* The node and what the core passed in. */
	node = private_data;
	owner = session;

	/* The session's pending work less its reservations not yet committed. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	reserved = 0U;
	for (slot = 0U; slot < AMDRDNA_NODE_MARKERS; slot++) {
		if (node->markers[slot].session == owner && node->markers[slot].state == AMDRDNA_ENTRY_RESERVED)
			reserved++;
	}

	/* What is still queued or running. */
	pending = owner->pending - reserved;

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Work of the session still queued or running. */
	if (pending != 0U)
		return EAGAIN;

	/* Work of the session's address space the GPU has not finished (a lost run that never ended). */
	quiet = amdrdna_space_quiet(owner->space);
	if (!quiet)
		return EAGAIN;

	/* Succeeded: the session's work has ended. */
	return 0;
}

/*
 * Isolates a session the core could not see stop: only one whose run was
 * given up (its space lost) and of which nothing runs on the GPU any more
 * (its queue was reset).  It takes no work from now on, and what it held
 * stays the device's (its blobs' VRAM is kept when they are destroyed).
 * EIO for any other: the device then faults.
 *
 * isolate is 0 only for a session whose queue reset and remap succeeded.
 */
static int
runtime_isolate(
	void *private_data,
	void *session)
{
	struct amdrdna_node *node;
	struct amdrdna_session *owner;
	unsigned long enabled;
	bool lost;
	bool quiet;

	/* The node and what the core passed in. */
	node = private_data;
	owner = session;

	/* A lost space of which nothing runs. */
	lost = amdrdna_space_lost(owner->space);
	quiet = amdrdna_space_quiet(owner->space);
	if (!lost || !quiet)
		return EIO;

	/* No more work, and what it holds is kept. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	owner->stopping = 1;
	owner->isolated = 1;

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Succeeded: the session is isolated. */
	amdrdna_mark("gpu device: a session is isolated; what it held stays until the device is reset");
	return 0;
}

/*
 * Stops new work after a fault: frames and markers queued afterwards are
 * refused, the queued ones complete with EIO, and no address space runs
 * anything new.  Idempotent.
 */
static void
runtime_fault(
	void *private_data,
	int error)
{
	struct amdrdna_node *node;
	unsigned long enabled;
	bool first;

	/* The node and what the core passed in. */
	node = private_data;

	/* Sets the flag the queue's users look at; the first fault is shown. */
	enabled = spin_lock_irqsave(&node->queue_lock);

	first = false;
	if (node->faulted == 0)
		first = true;
	node->faulted = 1;
	waitq_wake_all(&node->available);

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* No address space runs anything new. */
	amdrdna_spaces_fault(node->device);

	/* The first fault is shown. */
	if (first)
		amdrdna_mark("gpu device: the device faulted (%d); it takes no more work", error);
}

/*
 * Resets the GPU for the GPU core's recovery, which asks only once every
 * old session has gone: a mode-1 reset, the post and the stages set up
 * again on the bring-up thread.  Returns 0 when the GPU runs again, or
 * the reset's error (the GPU stays faulted).
 */
static int
runtime_reset(
	void *private_data)
{
	struct amdrdna_node *node;
	int error;

	/* The node the core passed in. */
	node = private_data;

	/* The reset, run by the bring-up thread; this waits for it. */
	error = amdrdna_reset_request(node->device, AMDRDNA_RESET_RECOVERY, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the GPU is checked and runs again. */
	return 0;
}

/*
 * Runs the queue in order for the node's life: a frame through the
 * executor, then its completion; a marker's completion; then the entry is
 * retired and the session's pending count falls.
 */
static void
runtime_worker(
	void *argument)
{
	struct amdrdna_node *node;
	struct amdrdna_node_entry *entry;
	struct amdrdna_session *owner;
	struct drv_gpu_completion *completion;
	unsigned long enabled;
	uint64_t observed;
	bool parked;
	int faulted;
	int error;

	/* The node the worker serves. */
	node = argument;

	/* Takes entries one at a time, sleeping while the queue is empty. */
	for (;;) {
		enabled = spin_lock_irqsave(&node->queue_lock);

		while (node->head == NULL) {
			observed = waitq_sequence(&node->available);
			(void)waitq_sleep(&node->available, &node->queue_lock, observed, 0U, 0U);
		}

		/* Takes the head entry; it runs from now on. */
		entry = node->head;
		node->head = entry->next;
		if (node->head == NULL)
			node->tail = NULL;
		entry->next = NULL;
		entry->state = AMDRDNA_ENTRY_RUNNING;
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
		 * A marker whose session's runs have not all ended goes to the
		 * completer.  The entry leaves the worker's queue before its
		 * completion (it still counts as pending), a marker once it has
		 * taken its runs (runtime_park): the command its waiter sends
		 * next may run at once, and its run is not one the marker waits
		 * for.
		 */
		if (entry->kind == RUNTIME_ENTRY_MARKER && error == 0) {
			parked = runtime_park(node, entry);
			if (parked)
				continue;

			/* The runs ended: whether they did their work. */
			error = runtime_runs_wait(entry->session, entry->runs, RUNTIME_RUN_MS);
		} else {
			runtime_dequeued(node, owner);
		}

		/* Delivers the completion. */
		completion = entry->completion;
		drv_gpu_complete(completion, error);

		/* Retires the entry: a frame is freed, a marker is free again. */
		runtime_retire(node, entry);
	}
}

/* Takes an entry the worker ran (a frame, or a marker carrying an error) off its session's count of queued entries. */
static void
runtime_dequeued(
	struct amdrdna_node *node,
	struct amdrdna_session *owner)
{
	unsigned long enabled;

	enabled = spin_lock_irqsave(&node->queue_lock);

	owner->queued--;

	spin_unlock_irqrestore(&node->queue_lock, enabled);
}

/*
 * Completes the parked markers in the order they were parked, for the
 * node's life: each once its session's runs have ended, with EIO when
 * one did not end in time, did not do its work, or the device faulted.
 */
static void
runtime_completer(
	void *argument)
{
	struct amdrdna_node *node;
	struct amdrdna_node_entry *entry;
	unsigned long enabled;
	uint64_t observed;
	int faulted;
	int error;

	/* The node the completer serves. */
	node = argument;

	/* Takes the oldest parked marker, sleeping while none is parked. */
	for (;;) {
		enabled = spin_lock_irqsave(&node->queue_lock);

		while (node->parked_head == NULL) {
			observed = waitq_sequence(&node->parked_more);
			(void)waitq_sleep(&node->parked_more, &node->queue_lock, observed, 0U, 0U);
		}

		/* The head stays parked (counted pending) until it is completed. */
		entry = node->parked_head;
		faulted = node->faulted;

		spin_unlock_irqrestore(&node->queue_lock, enabled);

		/* Waits for its runs, unless the device faulted. */
		error = EIO;
		if (faulted == 0)
			error = runtime_runs_wait(entry->session, entry->runs, RUNTIME_RUN_MS);

		/* Delivers the completion. */
		drv_gpu_complete(entry->completion, error);

		/* Takes it off the parked list, then retires it. */
		enabled = spin_lock_irqsave(&node->queue_lock);

		node->parked_head = entry->next;
		if (node->parked_head == NULL)
			node->parked_tail = NULL;
		entry->next = NULL;

		spin_unlock_irqrestore(&node->queue_lock, enabled);

		runtime_retire(node, entry);
	}
}

/*
 * Parks a marker the worker took when its session's runs have not all
 * ended, or when markers are parked already (markers complete in order).
 * Returns true when parked: the completer completes it.
 */
static bool
runtime_park(
	struct amdrdna_node *node,
	struct amdrdna_node_entry *entry)
{
	unsigned long enabled;
	unsigned queue;
	bool running;
	bool passed;

	/* The session's newest runs, as its frames before the marker left them. */
	mutex_lock(&node->lock);

	kern_memcpy(entry->runs, entry->session->runs, sizeof(entry->runs));

	mutex_unlock(&node->lock);

	/* Whether any of them still runs. */
	running = false;
	for (queue = 0U; queue < AMDRDNA_NODE_QUEUES; queue++) {
		passed = amdrdna_space_passed(entry->session->space, &entry->runs[queue]);
		if (!passed)
			running = true;
	}

	/*
	 * It leaves the worker's queue, with its runs taken (before the
	 * completer may retire it); then it parks behind the parked ones when
	 * it must wait, or when others wait already.
	 */
	enabled = spin_lock_irqsave(&node->queue_lock);

	entry->session->queued--;
	if (!running && node->parked_head == NULL) {
		spin_unlock_irqrestore(&node->queue_lock, enabled);
		return false;
	}
	entry->state = AMDRDNA_ENTRY_PARKED;
	entry->next = NULL;
	if (node->parked_tail == NULL) {
		node->parked_head = entry;
	} else {
		node->parked_tail->next = entry;
	}
	node->parked_tail = entry;
	waitq_wake_one(&node->parked_more);

	spin_unlock_irqrestore(&node->queue_lock, enabled);

	/* Succeeded: the completer has it. */
	return true;
}

/*
 * Waits up to milliseconds for each of a session's runs to end.  Returns
 * 0 when all did their work, EIO when one did not end in time (its space
 * is then lost and its queue reset) or did not do its work.
 */
static int
runtime_runs_wait(
	struct amdrdna_session *session,
	const struct amdrdna_space_ticket *runs,
	unsigned milliseconds)
{
	unsigned queue;
	int failed;
	int error;

	/* Each queue's run in turn. */
	failed = 0;
	for (queue = 0U; queue < AMDRDNA_NODE_QUEUES; queue++) {
		error = amdrdna_space_wait(session->space, &runs[queue], milliseconds);
		if (error != 0)
			failed = EIO;
	}

	/* Reports a run that failed. */
	if (failed != 0)
		return EIO;

	/* Succeeded: every run did its work. */
	return 0;
}

/*
 * Retires an entry whose completion was delivered: the session's pending
 * count falls, a frame is freed and a marker is free again (capacity).
 */
static void
runtime_retire(
	struct amdrdna_node *node,
	struct amdrdna_node_entry *entry)
{
	unsigned long enabled;
	uint32_t kind;

	/* The session's count, and a marker free again. */
	kind = entry->kind;
	enabled = spin_lock_irqsave(&node->queue_lock);

	entry->session->pending--;
	if (kind == RUNTIME_ENTRY_MARKER) {
		entry->state = AMDRDNA_ENTRY_FREE;
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
		drv_gpu_capacity_changed(node->gpu);
	}
}

/*
 * Runs a frame of a session through the bound executor under the node's
 * lock.  Returns the executor's error, ENOTSUP without an executor.
 */
static int
runtime_execute(
	struct amdrdna_node *node,
	struct amdrdna_session *session,
	const void *wire,
	uint32_t bytes)
{
	int error;

	/* Nothing runs frames before an executor is bound. */
	if (node->execute == NULL)
		return ENOTSUP;

	/* Runs the frame. */
	mutex_lock(&node->lock);

	error = node->execute(session, wire, bytes);

	mutex_unlock(&node->lock);

	/* Reports a frame the executor refused or that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the frame ran. */
	return 0;
}

/* Appends an entry to the queue and wakes the worker; the caller holds queue_lock. */
static void
runtime_enqueue(
	struct amdrdna_node *node,
	struct amdrdna_node_entry *entry)
{
	/* At the tail, counted in its session's queue. */
	entry->state = AMDRDNA_ENTRY_QUEUED;
	entry->session->queued++;
	entry->next = NULL;
	if (node->tail == NULL) {
		node->head = entry;
	} else {
		node->tail->next = entry;
	}

	/* The entry is the new tail. */
	node->tail = entry;

	/* The worker takes it. */
	waitq_wake_one(&node->available);
}

/* Finds the marker a token names for the session and completion, or NULL; the caller holds queue_lock. */
static struct amdrdna_node_entry *
runtime_marker(
	struct amdrdna_node *node,
	struct amdrdna_session *session,
	void *token,
	struct drv_gpu_completion *completion)
{
	struct amdrdna_node_entry *entry;
	uint32_t slot;

	/* Compares the token with each marker before using it. */
	for (slot = 0U; slot < AMDRDNA_NODE_MARKERS; slot++) {
		entry = &node->markers[slot];
		if (token != (void *)entry)
			continue;
		if (entry->session != session || entry->completion != completion)
			return NULL;
		if (entry->state == AMDRDNA_ENTRY_FREE)
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
	const struct amdrdna_node *node)
{
	unsigned count;
	uint32_t slot;

	/* Each free one. */
	count = 0U;
	for (slot = 0U; slot < AMDRDNA_NODE_MARKERS; slot++) {
		if (node->markers[slot].state == AMDRDNA_ENTRY_FREE)
			count++;
	}

	/* Succeeded: the count. */
	return count;
}

/* Reports whether a queue of the session's executor owns the timeline. */
static bool
runtime_timeline_owned(
	struct amdrdna_node *node,
	struct amdrdna_session *session,
	uint32_t timeline)
{
	uint64_t timelines;

	/* Without an executor no queue owns a timeline. */
	if (node->timelines == NULL)
		return false;

	/* The executor records the timelines its queues were made with. */
	timelines = node->timelines(session);
	if ((timelines & (1ULL << timeline)) == 0U)
		return false;

	/* Succeeded: the timeline is the session's. */
	return true;
}

/* Stores a 32-bit word into bytes at an offset, lowest byte first. */
static void
runtime_put32(
	uint8_t *bytes,
	uint32_t at,
	uint32_t value)
{
	uint32_t index;

	/* One byte at a time. */
	for (index = 0U; index < RUNTIME_WORD_BYTES; index++)
		bytes[at + index] = (uint8_t)(value >> (index * RUNTIME_BYTE_BITS));
}
