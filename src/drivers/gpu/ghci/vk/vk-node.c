/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The node's side of the Vulkan executor: what the executor adds to the
 * device's operations at the attach (the capset libvulkan reads, the
 * command frames run at once or queued with a completion, the job
 * markers on the queues' timelines, and the GPU core's recovery), and
 * the executor's own end at the detach.
 *
 * The GPU core's recovery: the stop of a node session is answered from
 * what the interrupt's lock guards (the session's completions and last
 * tickets) without waiting; an isolation fails the session's
 * completions and isolates its GHCI sessions, whose memory the device
 * keeps until a reset; the core's fault makes the device lost, and its
 * reset has the service thread reset the device.
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>
#include <uapi/gpu-fence.h>
#include <uapi/gpu-job.h>

#include "vk.h"

/* The capset libvulkan asks for: the native renderer's protocol record. */
#define NODE_CAPSET_ID			4U

/* The capabilities the executor adds to the device's. */
#define NODE_CAPABILITIES		(GPU_CAP_CAPSET | GPU_CAP_COMMAND | GPU_CAP_NOTIFICATION | GPU_CAP_FENCE | \
					 GPU_CAP_JOB | GPU_CAP_JOB_CAPACITY)

/* The shortfalls the attach's log names at most. */
#define NODE_SHORTFALLS			8U

static int node_capset(void *private_data, void *opaque, struct gpu_capset *capset);
static int node_command(void *private_data, void *opaque, const void *wire, uint32_t bytes);
static int node_submit(void *private_data, void *opaque, const void *wire, uint32_t bytes, uint32_t flags, uint32_t timeline, struct drv_gpu_completion *completion);
static void node_drain(void *private_data, void *opaque);
static int node_reserve(void *private_data, void *opaque, uint32_t timeline, struct drv_gpu_completion *completion, void **token);
static int node_commit(void *private_data, void *opaque, void *token, struct drv_gpu_completion *completion);
static int node_cancel(void *private_data, void *opaque, void *token, struct drv_gpu_completion *completion, unsigned fault);
static int node_capacity(void *private_data, void *opaque, uint32_t timeline, unsigned *available);
static int node_stop_begin(void *private_data, void *opaque, int error);
static int node_stop_poll(void *private_data, void *opaque);
static void node_fault(void *private_data, int error);
static int node_reset(void *private_data);
static int node_isolate(void *private_data, void *opaque);
static void node_detach(struct ghci_device *device);
static struct ghci_vk_session *session_of(void *opaque);
static struct ghci_vk_device *timeline_device(struct ghci_vk_session *session, uint32_t timeline);

/* The queued frames; the table lives as long as the kernel. */
static const struct drv_gpu_command_ops command_operations = {
	node_submit,
	node_drain
};

/* The job markers; the table lives as long as the kernel. */
static const struct drv_gpu_job_ops job_operations = {
	node_reserve,
	node_commit,
	node_cancel,
	node_capacity,
	NULL
};

/* The GPU core's recovery calls; the table lives as long as the kernel. */
static const struct drv_gpu_recovery_ops recovery_operations = {
	node_stop_begin,
	node_stop_poll,
	node_fault,
	node_reset,
	node_isolate
};

/* What the core of the driver asks of the executor; the table lives as long as the kernel. */
static const struct ghci_executor_ops executor_operations = {
	ghci_vk_session_open,
	ghci_vk_session_close,
	ghci_vk_memory_blob,
	ghci_vk_service,
	node_detach
};

/*
 * Attaches the Vulkan executor to a device before its node is published:
 * reads the format features (FMT), adds the executor's operations and
 * capabilities, and logs what the device lacks of Vulkan 1.0.  Returns
 * 0, or ENOMEM.
 */
int
ghci_vk_attach(
	struct ghci_device *device)
{
	const char *names[NODE_SHORTFALLS];
	struct ghci_vk *vk;
	unsigned count;
	unsigned index;

	/* The executor's record. */
	vk = kern_calloc(1U, sizeof(*vk));
	if (vk == NULL)
		return ENOMEM;
	vk->device = device;
	waitq_init(&vk->completing_wake, "ghci vk completing");

	/* The features of every format, read once. */
	ghci_vk_formats_read(device, &vk->formats);

	/* The operations, and the capabilities they make good. */
	device->vk = vk;
	device->executor_ops = &executor_operations;
	device->ops.get_capset = node_capset;
	device->ops.command = node_command;
	device->ops.commands = &command_operations;
	device->ops.jobs = &job_operations;
	device->ops.recovery = &recovery_operations;
	device->ops.capabilities |= NODE_CAPABILITIES;

	/* What a Vulkan 1.0 device should have and this one lacks; it is refused when asked for. */
	count = ghci_vk_caps_shortfalls(&device->caps, &vk->formats, names, NODE_SHORTFALLS);
	for (index = 0U; index < count; index++)
		kern_logf("ghci: %s: Vulkan 1.0 shortfall: %s\n", device->name, names[index]);

	/* Succeeded: the device runs Vulkan. */
	kern_logf("ghci: %s: Vulkan executor (formats from %s)\n",
	    device->name,
	    vk->formats.from_registers ? "FMT" : "the table");
	return 0;
}

/* Reports the native renderer's capset: id 4, version 0. */
static int
node_capset(
	void *private_data,
	void *opaque,
	struct gpu_capset *capset)
{
	(void)private_data;
	(void)opaque;

	/* Only the protocol's capset, version 0, in a reply that holds it. */
	if (capset->capset_id != NODE_CAPSET_ID || capset->capset_version != 0U)
		return ENOTSUP;
	if (capset->capacity < GHCI_CAPSET_BYTES || capset->capacity > GPU_CAPSET_MAX)
		return EINVAL;

	/* The record, and nothing after it. */
	kern_memset(capset->data, 0, sizeof(capset->data));
	ghci_capset_fill(capset->data);
	capset->bytes = GHCI_CAPSET_BYTES;

	/* Succeeded: the capset is reported. */
	return 0;
}

/* Runs one frame at once. */
static int
node_command(
	void *private_data,
	void *opaque,
	const void *wire,
	uint32_t bytes)
{
	struct ghci_vk_session *session;
	int error;

	(void)private_data;

	/* Whole words, within the command's bound. */
	session = session_of(opaque);
	if (bytes == 0U || bytes > GPU_COMMAND_MAX || (bytes & 3U) != 0U)
		return EINVAL;

	/* The frame through the executor. */
	error = ghci_vk_stream_execute(session, wire, bytes);
	if (error != 0)
		return error;

	/* Succeeded: the frame ran. */
	return 0;
}

/*
 * Runs one frame and queues its completion: a frame of timeline 0 (the
 * decoder's notification) completes once it has run, a context fence of
 * a timeline once the work of the timeline's device placed until now has
 * finished.  A frame of no bytes is a fence alone.
 */
static int
node_submit(
	void *private_data,
	void *opaque,
	const void *wire,
	uint32_t bytes,
	uint32_t flags,
	uint32_t timeline,
	struct drv_gpu_completion *completion)
{
	struct ghci_vk_session *session;
	struct ghci_vk_frame *frame;
	int error;

	(void)private_data;

	/* Only the context's fence flag; a timeline only with it. */
	session = session_of(opaque);
	if ((flags & ~GPU_COMMAND_CONTEXT_FENCE) != 0U || timeline >= GHCI_VK_TIMELINES)
		return EINVAL;
	if (flags == 0U && timeline != 0U)
		return EINVAL;

	/* Whole words within the bound; an empty frame only as a fence. */
	if (bytes > GPU_COMMAND_MAX || (bytes & 3U) != 0U)
		return EINVAL;
	if (bytes == 0U && flags == 0U)
		return EINVAL;

	/* The completion's record before the frame runs, on timeline 0 or one of the session's queues. */
	error = ghci_vk_frame_new(session, timeline, completion, &frame);
	if (error != 0)
		return error;

	/* The frame runs now; its replies are written before the completion is queued. */
	if (bytes != 0U) {
		error = ghci_vk_stream_execute(session, wire, bytes);
		if (error != 0) {
			kern_free(frame);
			return error;
		}
	}

	/* The completion, queued (a stopped session frees the record). */
	error = ghci_vk_frame_queue(session, frame);
	if (error != 0)
		return error;

	/* Succeeded: the service thread completes the frame. */
	return 0;
}

/* Waits until every queued frame and committed marker of a closing node session has ended. */
static void
node_drain(
	void *private_data,
	void *opaque)
{
	/* The session retires its own completions as their tickets are answered. */
	ghci_vk_drain(private_data, session_of(opaque));
}

/* Reserves a job marker on a timeline of the session's queues: EAGAIN when all are in use. */
static int
node_reserve(
	void *private_data,
	void *opaque,
	uint32_t timeline,
	struct drv_gpu_completion *completion,
	void **token)
{
	struct ghci_device *device;
	struct ghci_vk_session *session;
	struct ghci_vk_device *owner;
	unsigned long irq;
	unsigned index;
	int error;

	/* A timeline of the session's queues on a device that runs. */
	device = private_data;
	session = session_of(opaque);
	*token = NULL;
	irq = spin_lock_irqsave(&device->irq_lock);

	owner = timeline_device(session, timeline);
	error = EINVAL;
	if (owner != NULL && (session->stopping || session->isolated || device->state == GHCI_STATE_LOST)) {
		error = EIO;
	} else if (owner != NULL) {
		error = ghci_jobs_reserve(&session->jobs, completion, &index);
	}
	if (error == 0) {
		session->marker_timeline[index] = timeline;
		*token = &session->jobs.markers[index];
	}

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* No marker. */
	if (error != 0)
		return error;

	/* Succeeded: the marker is reserved. */
	return 0;
}

/*
 * Commits a reserved marker to the last submission of its timeline's
 * device, placed or held back: it completes when that work has finished
 * (or a reset ended it, or the held work failed), at once when there is
 * none.
 */
static int
node_commit(
	void *private_data,
	void *opaque,
	void *token,
	struct drv_gpu_completion *completion)
{
	enum ghci_ticket_state state;
	struct ghci_device *device;
	struct ghci_vk_session *session;
	struct ghci_vk_device *owner;
	struct ghci_ticket ticket;
	unsigned long irq;
	unsigned index;
	int passed;
	int held;
	int error;

	/* The marker the token names, bound under the interrupt's lock. */
	device = private_data;
	session = session_of(opaque);
	passed = 0;
	irq = spin_lock_irqsave(&device->irq_lock);

	error = ghci_jobs_index(&session->jobs, token, &index);
	if (error == 0) {
		/* The work to wait for: the timeline's device's last, none when it finished. */
		ticket.seq = 0U;
		ticket.generation = 0U;
		owner = timeline_device(session, session->marker_timeline[index]);
		if (owner != NULL && owner->submitted) {
			state = ghci_cp_ticket_locked(device, &owner->last);
			if (state != GHCI_TICKET_DONE)
				ticket = owner->last;
		}
		held = 0;
		if (owner != NULL)
			held = ghci_vk_held_ticket(owner, &ticket);
		session->marker_device[index] = NULL;
		if (held)
			session->marker_device[index] = owner;
		error = ghci_jobs_commit(&session->jobs, index, completion, ticket.seq, ticket.generation, &passed);
	}

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* A token or a completion that is not this reservation's. */
	if (error != 0)
		return error;

	/* Work that has already finished completes the marker now; the rest is the service thread's. */
	if (passed)
		drv_gpu_complete(completion, 0);
	else
		ghci_service_wake(device, GHCI_WORK_RECLAIM);

	/* Succeeded: the marker completes in order. */
	return 0;
}

/* Withdraws a reservation (normal) or ends an uncertain one with an error at the next retirement (fault). */
static int
node_cancel(
	void *private_data,
	void *opaque,
	void *token,
	struct drv_gpu_completion *completion,
	unsigned fault)
{
	struct ghci_device *device;
	struct ghci_vk_session *session;
	unsigned long irq;
	unsigned index;
	int error;

	/* The marker the token names, changed under the interrupt's lock. */
	device = private_data;
	session = session_of(opaque);
	irq = spin_lock_irqsave(&device->irq_lock);

	error = ghci_jobs_index(&session->jobs, token, &index);
	if (error == 0)
		error = ghci_jobs_cancel(&session->jobs, index, completion, fault);

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* A cancel the marker's state does not allow. */
	if (error != 0)
		return error;

	/* A failed marker completes with the next round of the service. */
	if (fault != 0U)
		ghci_service_wake(device, GHCI_WORK_RECLAIM);

	/* Succeeded: the reservation is withdrawn or ended. */
	return 0;
}

/* Reports how many markers a reservation on the timeline could take now, without sleeping. */
static int
node_capacity(
	void *private_data,
	void *opaque,
	uint32_t timeline,
	unsigned *available)
{
	struct ghci_device *device;
	struct ghci_vk_session *session;
	struct ghci_vk_device *owner;
	unsigned long irq;
	int error;

	/* A timeline of the session's queues on a device that runs. */
	device = private_data;
	session = session_of(opaque);
	*available = 0U;
	irq = spin_lock_irqsave(&device->irq_lock);

	owner = timeline_device(session, timeline);
	error = EINVAL;
	if (owner != NULL && device->state == GHCI_STATE_LOST) {
		error = EIO;
	} else if (owner != NULL) {
		*available = ghci_jobs_capacity(&session->jobs);
		error = 0;
	}

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* A timeline the session's queues do not complete on. */
	if (error != 0)
		return error;

	/* Succeeded: the count is a snapshot, not a promise. */
	return 0;
}

/*
 * Begins the stop of a node session: it takes no new frame or job.  Its
 * work in the ring ends on the GPU or with a reset, and the service
 * thread, woken, ends the work it holds back; nothing waits here.
 */
static int
node_stop_begin(
	void *private_data,
	void *opaque,
	int error)
{
	struct ghci_device *device;
	struct ghci_vk_session *session;
	unsigned long irq;

	(void)error;

	/* The stop mark, which the frames and the jobs read. */
	device = private_data;
	session = session_of(opaque);
	irq = spin_lock_irqsave(&device->irq_lock);

	session->stopping = 1;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* The service thread ends what the session holds back. */
	ghci_service_wake(device, GHCI_WORK_RECLAIM);

	/* Succeeded: the poll says when the stop is done. */
	return 0;
}

/*
 * Reports whether a stopping node session's work is done: 0 once none of
 * its devices' work is pending in the GPU and every completion is
 * retired, EAGAIN before (the service thread retires meanwhile).
 */
static int
node_stop_poll(
	void *private_data,
	void *opaque)
{
	struct ghci_device *device;
	struct ghci_vk_session *session;
	unsigned long irq;
	int pending;

	/* What is still pending, under the interrupt's lock only. */
	device = private_data;
	session = session_of(opaque);
	irq = spin_lock_irqsave(&device->irq_lock);

	pending = ghci_vk_session_pending(session);

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Work still pending: the service thread retires it as the GPU passes it. */
	if (pending) {
		ghci_service_wake(device, GHCI_WORK_RECLAIM);
		return EAGAIN;
	}

	/* Succeeded: the GPU no longer reaches the session's memory for it. */
	return 0;
}

/* Takes the GPU core's fault: the device is lost from here; calling it again changes nothing. */
static void
node_fault(
	void *private_data,
	int error)
{
	(void)error;

	/* The device's loss, as a recovery that gives up makes it. */
	ghci_recover_fault(private_data);
}

/*
 * Runs the GPU core's reset once no older owner is left: the service
 * thread resets the device and checks it, and the memory a loss or an
 * isolation kept goes back.  Returns 0, or the error of the reset.
 */
static int
node_reset(
	void *private_data)
{
	int error;

	/* The reset on the service thread. */
	error = ghci_recover_reset(private_data);
	if (error != 0)
		return error;

	/* Succeeded: the device runs again. */
	return 0;
}

/*
 * Isolates a node session whose stop could not be confirmed: every
 * completion of it ends with an error before the isolation returns (one
 * the service thread is still reporting too), it takes no more work, and
 * each of its devices' GHCI sessions is isolated, so that its unmaps and
 * closes keep the memory with the device until a reset.  Other sessions
 * go on.  Returns 0.
 */
static int
node_isolate(
	void *private_data,
	void *opaque)
{
	struct ghci_device *device;
	struct ghci_vk_session *session;
	struct ghci_vk_device *each;
	struct ghci_vm *vms[GHCI_SESSIONS];
	unsigned long irq;
	unsigned count;
	unsigned index;

	/* The completions fail and the session's devices are noted, under the interrupt's lock. */
	device = private_data;
	session = session_of(opaque);
	count = 0U;
	irq = spin_lock_irqsave(&device->irq_lock);

	ghci_vk_session_fail(session);
	for (each = session->devices; each != NULL && count < GHCI_SESSIONS; each = each->next) {
		vms[count] = each->vm;
		count++;
	}

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* The failed completions end now, with any the service thread is still reporting. */
	ghci_vk_settle(device, session);

	/* Each GHCI session of the node session is isolated. */
	for (index = 0U; index < count; index++)
		ghci_vm_isolate(device, vms[index]);

	/* The service thread ends what the session holds back. */
	ghci_service_wake(device, GHCI_WORK_RECLAIM);

	/* Succeeded: the session is isolated. */
	return 0;
}

/* Lets the executor's record and its transfer programs go at the detach; no node session is left. */
static void
node_detach(
	struct ghci_device *device)
{
	struct ghci_vk *vk;
	unsigned kind;

	/* The driver's transfer programs. */
	vk = device->vk;
	for (kind = 0U; kind < GHCI_VK_META_KINDS; kind++) {
		ghci_gsi1_program_free(vk->meta[kind][0]);
		ghci_gsi1_program_free(vk->meta[kind][1]);
	}

	/* The record and the table. */
	kern_free(device->vk);
	device->vk = NULL;
	device->executor_ops = NULL;
}

/* Reports the executor's side of a node session the core passed. */
static struct ghci_vk_session *
session_of(
	void *opaque)
{
	struct ghci_session *owner;

	/* The node session's executor side, made at its open. */
	owner = opaque;
	return owner->vk;
}

/* Reports the device whose queue completes jobs on a timeline (1 to 63); NULL for none.  The caller holds irq_lock. */
static struct ghci_vk_device *
timeline_device(
	struct ghci_vk_session *session,
	uint32_t timeline)
{
	/* Timelines 1 to 63, as vkGetDeviceQueue2 gave them. */
	if (timeline == 0U || timeline >= GHCI_VK_TIMELINES)
		return NULL;

	/* Succeeded: the device, or NULL. */
	return session->timeline_device[timeline];
}
