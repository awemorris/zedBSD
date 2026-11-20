/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The queue of the Vulkan executor: the submission, the fences, the
 * semaphores and the events, and the completions of a node session (its
 * job markers and its queued frames) that the service thread retires as
 * the GPU passes their tickets.  A retirement counts what it took as the
 * session's completing until each is reported to the core, so that a
 * drain or a stop never says that a session is done while a completion
 * of it is still being reported.
 *
 * One ring is the one queue, so the submissions run in the order they
 * are placed: a semaphore needs no wait of its own, and a fence or a job
 * is done when the ticket of the last submission before it is.  A
 * submission places the packets of its command buffers (vk-execute.c)
 * in the device's GHCI session and a fence after them; one whose command
 * buffers record nothing places the fence alone.  An event is a word of
 * the device's data heap, which the host sets and resets here and the
 * GPU with a fence.  A query pool's slots are in the data heap too, and
 * the host reads its results there.
 *
 * A submission that waits for an event nothing earlier in it sets is
 * held back from its cut on (vk-execute.c), and so is every later
 * submission of the device, in order: the ring never waits for a word
 * the client writes, so a wait that never ends stops no other session
 * and not the display.  The kernel places a held submission's next
 * piece once the events of its cut are set and no placed work writes
 * them any more: when the host sets an event, when the client polls the
 * fence of held work and at the end of each of the client's frames (on
 * the client's thread, which may wait for the ring's room), and at each
 * round of the service thread, after each fence interrupt (which only
 * tries the session's hold_lock and enters only a free ring with room
 * now).  A fence, a job
 * marker or a frame behind held work completes once that work has run;
 * held work ends with an error when its session stops, is isolated or
 * closes, its device goes, the GPU is lost, or a reset ends the
 * device's placed work.
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/waitq.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "vk.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* How long one wait of the drain for a session's last ticket lasts before it looks again. */
#define DRAIN_WAIT_MS			100U

/* The bytes and the alignment of an event's word. */
#define EVENT_BYTES			8U

/* The bytes of a query's slot (its result and availability word) and the most queries of a pool. */
#define QUERY_SLOT_BYTES		16U
#define QUERY_POOL_MAX			65536U

/* The dwords a piece of a held submission adds to its packets: the binds, the barriers and the fence. */
#define PIECE_EXTRA_DWORDS		(2U * GHCI_DW_BIND_SESSION + 2U * GHCI_DW_BARRIER + GHCI_DW_FENCE)

static int queue_submit(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int submission_check(struct ghci_vk_session *session, const VkSubmitInfo *submits, uint64_t count, struct ghci_vk_device *device, int *recorded);
static int create_sync(struct ghci_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int destroy_sync(struct ghci_vk_session *session, enum ghci_vk_kind kind, struct i915_wire_reader *reader);
static int reset_fences(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int fence_status(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int event_operation(struct ghci_vk_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_query_pool(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int query_results(struct ghci_vk_session *session, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static enum ghci_job_answer judge(void *context, uint64_t seq, uint64_t generation);
static void retire_session(struct ghci_device *device, struct ghci_vk_session *session);
static unsigned retire(struct ghci_vk_session *session, void **done, int *errors, struct ghci_vk_session **owners, unsigned capacity, struct ghci_vk_frame **gone);
static void complete(struct ghci_device *device, void **done, const int *errors, struct ghci_vk_session **owners, unsigned count, struct ghci_vk_frame *gone);
static uint32_t hold_submit(struct ghci_vk_session *session, struct ghci_vk_device *device, struct ghci_vk_submission *submission, struct ghci_vk_object *fence);
static int hold_step(struct ghci_vk_session *session, struct ghci_vk_device *device, struct ghci_vk_hold *hold, int wait);
static int piece_end(struct ghci_vk_session *session, struct ghci_vk_hold *hold, int wait, uint32_t *end);
static int cut_ready(struct ghci_vk_session *session, const struct ghci_vk_hold *hold, const struct ghci_vk_cut *cut);
static int piece_place(struct ghci_vk_session *session, struct ghci_vk_device *device, struct ghci_vk_hold *hold, uint32_t end, int wait);
static int ring_place(struct ghci_device *gpu, struct ghci_vm *vm, const uint32_t *packets, uint32_t dwords, int head, int tail, int wait, struct ghci_ticket *ticket, int *placed);
static void hold_finish(struct ghci_vk_session *session, struct ghci_vk_device *device, struct ghci_vk_hold *hold);
static int holds_dead(struct ghci_vk_session *session, struct ghci_vk_device *device);
static void held_resolve(struct ghci_vk_session *session, struct ghci_vk_device *device, uint64_t serial, const struct ghci_ticket *ticket);
static void holds_service(struct ghci_device *gpu);

/*
 * Runs a submission, fence, semaphore or event command; *handled stays
 * clear for another group's number.
 */
int
ghci_vk_queue_route(
	struct ghci_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another group. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_QUEUE_SUBMIT:
		error = queue_submit(session, requested, reader, reply);
		break;
	case GPU_OP_CREATE_FENCE:
	case GPU_OP_CREATE_SEMAPHORE:
	case GPU_OP_CREATE_EVENT:
		error = create_sync(session, opcode, requested, reader, reply);
		break;
	case GPU_OP_DESTROY_FENCE:
		error = destroy_sync(session, GHCI_VK_FENCE, reader);
		break;
	case GPU_OP_DESTROY_SEMAPHORE:
		error = destroy_sync(session, GHCI_VK_SEMAPHORE, reader);
		break;
	case GPU_OP_DESTROY_EVENT:
		error = destroy_sync(session, GHCI_VK_EVENT, reader);
		break;
	case GPU_OP_RESET_FENCES:
		error = reset_fences(session, requested, reader, reply);
		break;
	case GPU_OP_GET_FENCE_STATUS:
		error = fence_status(session, requested, reader, reply);
		break;
	case GPU_OP_GET_EVENT_STATUS:
	case GPU_OP_SET_EVENT:
	case GPU_OP_RESET_EVENT:
		error = event_operation(session, opcode, requested, reader, reply);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* A command that could not be decoded or run. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran. */
	return 0;
}

/*
 * Retires, on the service thread, the completions of every node session
 * whose tickets the GPU passed or a reset ended, and completes them once
 * the interrupt's lock is let go.
 */
void
ghci_vk_service(
	struct ghci_device *device)
{
	struct ghci_vk_session *owners[GHCI_VK_DONE_BATCH];
	struct ghci_vk_session *session;
	struct ghci_vk_frame *gone;
	struct ghci_vk *vk;
	void *done[GHCI_VK_DONE_BATCH];
	int errors[GHCI_VK_DONE_BATCH];
	unsigned long irq;
	unsigned count;

	/* The held submissions that may go now, so that what failed is retired below. */
	holds_service(device);

	/* Batches of completions until a batch is not full. */
	vk = device->vk;
	do {
		/* Each session's markers and frames that are done. */
		count = 0U;
		gone = NULL;
		irq = spin_lock_irqsave(&device->irq_lock);

		for (session = vk->sessions; session != NULL && count < GHCI_VK_DONE_BATCH; session = session->next)
			count += retire(session, done + count, errors + count, owners + count, GHCI_VK_DONE_BATCH - count, &gone);

		spin_unlock_irqrestore(&device->irq_lock, irq);

		/* The core hears of each. */
		complete(device, done, errors, owners, count, gone);
	} while (count == GHCI_VK_DONE_BATCH);
}

/*
 * Ends every completion of a node session before it closes: its held
 * submissions fail, the session retires its own completions as their
 * tickets are answered, waiting a bounded time on its last ticket
 * between looks, and then waits until no completion another thread took
 * is still being reported to the core and no service round looks at the
 * session.  A ticket that never passes is ended by the recovery (a hang,
 * a reset, the loss of the device), so the drain ends.  The caller holds
 * none of the device's locks.
 */
void
ghci_vk_drain(
	struct ghci_device *device,
	struct ghci_vk_session *session)
{
	struct ghci_vk_device *each;
	struct ghci_ticket last;
	struct ghci_vk *vk;
	unsigned long irq;
	uint64_t sequence;
	int pending;
	int submitted;

	/* Held submissions end with an error: nothing sets what they wait for once the session closes. */
	mutex_lock(&session->hold_lock);

	for (each = session->devices; each != NULL; each = each->next)
		ghci_vk_holds_fail(session, each);

	mutex_unlock(&session->hold_lock);

	/* Looks until nothing of the session is pending, being reported or looked at by the service. */
	vk = device->vk;
	for (;;) {
		/* The session's completions that are done, reported at once. */
		retire_session(device, session);

		/* What is left; with nothing, the completions another thread is still reporting. */
		irq = spin_lock_irqsave(&device->irq_lock);

		pending = session->jobs.committed != 0U || session->frame_count != 0U;
		while (!pending && (session->completing != 0U || session->holding != 0U)) {
			sequence = waitq_sequence(&vk->completing_wake);
			(void)waitq_sleep(&vk->completing_wake, &device->irq_lock, sequence, 0U, 0U);
		}
		last = session->last;
		submitted = session->submitted;

		spin_unlock_irqrestore(&device->irq_lock, irq);

		/* Nothing left: no completion of the session is in flight. */
		if (!pending)
			return;

		/* Waits for the session's last work; a failed marker or frame needs no wait. */
		if (submitted)
			(void)ghci_cp_wait(device, &last, DRAIN_WAIT_MS);
		else
			ghci_sleep_ms(DRAIN_WAIT_MS);
	}
}

/*
 * Ends the completions of a node session that are done now (after an
 * isolation, every one), and waits until none that another thread took
 * is still being reported to the core.  It waits for no ticket.  The
 * caller holds none of the device's locks.
 */
void
ghci_vk_settle(
	struct ghci_device *device,
	struct ghci_vk_session *session)
{
	struct ghci_vk *vk;
	unsigned long irq;
	uint64_t sequence;

	/* What is done, reported at once. */
	vk = device->vk;
	retire_session(device, session);

	/* What the service thread took is reported before the caller goes on. */
	irq = spin_lock_irqsave(&device->irq_lock);

	while (session->completing != 0U) {
		sequence = waitq_sequence(&vk->completing_wake);
		(void)waitq_sleep(&vk->completing_wake, &device->irq_lock, sequence, 0U, 0U);
	}

	spin_unlock_irqrestore(&device->irq_lock, irq);
}

/*
 * Makes the record of a frame's completion before the frame runs, so
 * that a frame that ran always has its completion queued.  timeline is 0
 * (the decoder's notification) or a timeline a queue of the session
 * completes on.  Returns 0 with the record, EINVAL for a timeline no
 * queue of the session owns, or ENOMEM.
 */
int
ghci_vk_frame_new(
	struct ghci_vk_session *session,
	uint32_t timeline,
	struct drv_gpu_completion *completion,
	struct ghci_vk_frame **result)
{
	struct ghci_vk_frame *frame;
	unsigned long irq;
	int known;

	/* Timeline 0, or one of the session's queues. */
	known = 0;
	if (timeline == 0U) {
		known = 1;
	} else if (timeline < GHCI_VK_TIMELINES) {
		irq = spin_lock_irqsave(&session->device->irq_lock);

		if (session->timeline_device[timeline] != NULL)
			known = 1;

		spin_unlock_irqrestore(&session->device->irq_lock, irq);
	}
	if (!known)
		return EINVAL;

	/* The record. */
	frame = kern_calloc(1U, sizeof(*frame));
	if (frame == NULL)
		return ENOMEM;
	frame->completion = completion;
	frame->timeline = timeline;

	/* Succeeded: the caller runs the frame, then queues the record. */
	*result = frame;
	return 0;
}

/*
 * Queues a frame's completion once the frame has run: a frame of
 * timeline 0 completes at the next retirement, since its replies are
 * already written; a context fence of a timeline waits for the work its
 * device placed or holds until now (none when the frame destroyed the
 * device), and ends with an error when held work of the device failed
 * since the timeline's last context fence that reported it, or the
 * device is lost.
 * Returns 0, or EIO for a session the GPU core stopped or isolated, and
 * then frees the record.
 */
int
ghci_vk_frame_queue(
	struct ghci_vk_session *session,
	struct ghci_vk_frame *frame)
{
	struct ghci_vk_frame **position;
	struct ghci_vk_device *device;
	unsigned long irq;
	int stopped;
	int held;

	/* The ticket it waits for, and the frame last in the session's queue. */
	irq = spin_lock_irqsave(&session->device->irq_lock);

	stopped = session->stopping || session->isolated;
	device = NULL;
	if (frame->timeline != 0U)
		device = session->timeline_device[frame->timeline];
	if (device != NULL && device->submitted)
		frame->ticket = device->last;
	held = 0;
	if (device != NULL)
		held = ghci_vk_held_ticket(device, &frame->ticket);
	if (held)
		frame->held = device;
	if (device != NULL && (device->failed_timelines & ((uint64_t)1U << frame->timeline)) != 0U) {
		/* Held work it follows failed since the timeline's last context fence. */
		frame->failed = 1;
		device->failed_timelines &= ~((uint64_t)1U << frame->timeline);
	}
	if (device != NULL && device->lost)
		frame->failed = 1;
	if (!stopped) {
		position = &session->frames;
		while (*position != NULL)
			position = &(*position)->next;
		*position = frame;
		session->frame_count++;
	}

	spin_unlock_irqrestore(&session->device->irq_lock, irq);

	/* A stopped session takes no completion. */
	if (stopped) {
		kern_free(frame);
		return EIO;
	}

	/* Succeeded: the service thread completes the frame. */
	ghci_service_wake(session->device, GHCI_WORK_RECLAIM);
	return 0;
}

/* Records the ticket of a device's last submission, which its jobs, frames and stop wait for. */
void
ghci_vk_submitted(
	struct ghci_vk_device *device,
	const struct ghci_ticket *ticket)
{
	struct ghci_vk_session *session;
	unsigned long irq;

	/* The device's and the session's last work. */
	session = device->session;
	irq = spin_lock_irqsave(&session->device->irq_lock);

	device->last = *ticket;
	device->submitted = 1;
	session->last = *ticket;
	session->submitted = 1;

	spin_unlock_irqrestore(&session->device->irq_lock, irq);
}

/*
 * Reports whether a node session still has work or completions the GPU
 * has not answered: 1 when it has.  The caller holds irq_lock.
 */
int
ghci_vk_session_pending(
	struct ghci_vk_session *session)
{
	enum ghci_ticket_state state;
	struct ghci_vk_device *device;

	/* Completions not retired yet, or retired and not yet reported to the core, or held work. */
	if (session->jobs.committed != 0U ||
	    session->frame_count != 0U ||
	    session->completing != 0U ||
	    session->hold_count != 0U)
		return 1;

	/* A device whose last work the GPU has not finished. */
	for (device = session->devices; device != NULL; device = device->next) {
		if (!device->submitted)
			continue;
		state = ghci_cp_ticket_locked(session->device, &device->last);
		if (state == GHCI_TICKET_PENDING)
			return 1;
	}

	/* Succeeded: nothing pending. */
	return 0;
}

/*
 * Ends every committed marker and queued frame of a node session with an
 * error at the next retirement, and isolates the session from new work.
 * The caller holds irq_lock.
 */
void
ghci_vk_session_fail(
	struct ghci_vk_session *session)
{
	struct ghci_vk_frame *frame;

	/* The markers and the frames fail; nothing new is taken. */
	ghci_jobs_fail(&session->jobs);
	for (frame = session->frames; frame != NULL; frame = frame->next)
		frame->failed = 1;
	session->isolated = 1;
}

/*
 * Places what the device holds back as far as it may go now, oldest
 * first: each held submission's pieces whose events are set, and each
 * whole one after it.  With wait (a client's thread) a piece goes
 * through the ring's gate as any submission does and may wait for room;
 * without (the service thread) only into a free ring with room now, and
 * the rest waits for the next round.  Held work of a device whose
 * session stopped or was isolated, whose placed work a reset ended, or
 * whose GPU is lost, and held work after a piece that failed, fails.
 * The caller holds the session's hold_lock.  Returns 0, or the error
 * with which the held work failed.
 */
int
ghci_vk_holds_advance(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device,
	int wait)
{
	struct ghci_vk_hold *hold;
	int dead;
	int error;

	/* The oldest held submission, until one waits. */
	while (device->holds != NULL) {
		/* A device whose work can no longer run. */
		dead = holds_dead(session, device);
		if (dead) {
			ghci_vk_holds_fail(session, device);
			return EIO;
		}

		/* Its pieces that may go; one that waits keeps it and every later one held. */
		hold = device->holds;
		error = hold_step(session, device, hold, wait);
		if (error == EAGAIN)
			return 0;
		if (error != 0) {
			ghci_vk_holds_fail(session, device);
			return error;
		}

		/* Placed whole: it is no longer held. */
		device->holds = hold->next;
		hold_finish(session, device, hold);
	}

	/* Succeeded: nothing more may go now. */
	return 0;
}

/*
 * Ends every submission a device holds back with an error: its VkFence
 * is lost, the job markers and frames behind it fail, and its memory
 * goes back (after its last piece, for one placed in part).  The device
 * is lost from then on: what it is given later fails too, so that a wait
 * after the failure (vkQueueWaitIdle behind a submission with no fence)
 * does not succeed.  The caller holds the session's hold_lock.
 */
void
ghci_vk_holds_fail(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device)
{
	struct ghci_vk_object *object;
	struct ghci_vk_hold *hold;
	unsigned long irq;
	uint32_t timeline;
	unsigned count;

	/* Each held submission, oldest first. */
	count = 0U;
	while (device->holds != NULL) {
		hold = device->holds;
		device->holds = hold->next;
		count++;

		/* The fence it would have signaled, still waiting for it (a reset of the fence lets it go). */
		object = ghci_vk_object_find(session, GHCI_VK_FENCE, hold->fence);
		if (object != NULL && object->as.fence.held) {
			object->as.fence.held = 0;
			object->as.fence.lost = 1;
		}

		/* Its memory: after its last piece, or at once when nothing of it was placed; and the budget it took. */
		if (hold->ticketed)
			ghci_vk_submission_end(device, &hold->submission, &hold->ticket, 1);
		else
			ghci_vk_submission_end(device, &hold->submission, NULL, 0);
		ghci_vk_budget_give(session, hold->charged);
		kern_free(hold);
	}
	if (count == 0U)
		return;

	/*
	 * Nothing is held; what waited for it fails, and the next context
	 * fence of each of the device's timelines that no failed frame
	 * reported fails too.
	 */
	irq = spin_lock_irqsave(&session->device->irq_lock);

	session->hold_count -= device->hold_count;
	device->hold_count = 0U;
	device->lost = 1;
	for (timeline = 1U; timeline < GHCI_VK_TIMELINES; timeline++) {
		if (session->timeline_device[timeline] == device)
			device->failed_timelines |= (uint64_t)1U << timeline;
	}
	held_resolve(session, device, 0U, NULL);

	spin_unlock_irqrestore(&session->device->irq_lock, irq);

	/* The failures are retired at the service's next round. */
	kern_logf("ghci: %s: %u held submission(s) of a session end with an error\n", session->device->name, count);
	ghci_service_wake(session->device, GHCI_WORK_RECLAIM);
}

/*
 * Makes the ticket a job marker or a frame of the device's timeline
 * waits for while the device holds submissions back: the serial of the
 * last one held, in GHCI_VK_HELD_GENERATION.  The caller holds irq_lock.
 * Returns 1 with the ticket, or 0 (and the ticket unchanged) when
 * nothing is held.
 */
int
ghci_vk_held_ticket(
	struct ghci_vk_device *device,
	struct ghci_ticket *ticket)
{
	/* Nothing held. */
	if (device->hold_count == 0U)
		return 0;

	/* Succeeded: the last held submission. */
	ticket->seq = device->hold_serial;
	ticket->generation = GHCI_VK_HELD_GENERATION;
	return 1;
}

/*
 * vkQueueSubmit: [queue][u32 count][u64 count][count x VkSubmitInfo]
 * [fence] -> [result].  The submissions' command buffers run in the
 * device's GHCI session, with a fence after them that the VkFence, the
 * jobs and the frames wait for.  One that waits for an event, or comes
 * while the device holds submissions back, is held from its first such
 * wait on (hold_submit).
 */
static int
queue_submit(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct ghci_vk_object *queue;
	struct ghci_vk_object *fence;
	struct ghci_vk_device *device;
	struct ghci_vk_submission submission;
	VkSubmitInfo *submits;
	struct ghci_ticket ticket;
	uint64_t handle;
	int recorded;
	int placed;
	int lost;
	uint64_t count;
	uint64_t index;
	uint64_t signal;
	uint32_t result;
	int error;

	/* The queue, the submissions and the fence. */
	handle = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = ghci_vk_read_count(reader);
	submits = i915_vkc_array(reader, &session->arena, count, sizeof(*submits));
	for (index = 0U; index < count && reader->error == 0; index++)
		i915_vkc_dec_VkSubmitInfo(reader, &session->arena, &submits[index]);
	signal = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A queue of the session, and a fence of its device. */
	queue = ghci_vk_object_find(session, GHCI_VK_QUEUE, handle);
	if (queue == NULL)
		return EINVAL;
	device = queue->device;
	fence = NULL;
	if (signal != 0U) {
		fence = ghci_vk_object_find(session, GHCI_VK_FENCE, signal);
		if (fence == NULL || fence->device != device)
			return EINVAL;
	}

	/* The semaphores and the command buffers named, ready to run. */
	error = submission_check(session, submits, count, device, &recorded);
	if (error != 0)
		return error;

	/* A lost device runs nothing more. */
	lost = __atomic_load_n(&device->lost, __ATOMIC_ACQUIRE);
	if (lost) {
		ghci_vk_reply_result(reply, requested, (uint32_t)VK_ERROR_DEVICE_LOST);
		return 0;
	}

	/* The recorded commands made into packets (none for buffers that record nothing). */
	kern_memset(&submission, 0, sizeof(submission));
	if (recorded) {
		error = ghci_vk_execute(session, device, submits, count, &submission);
		if (error != 0) {
			ghci_vk_submission_end(device, &submission, NULL, 0);
			return error;
		}
	}

	/* Behind held submissions, or waiting for an event: held, and placed as far as it may be now. */
	if (device->holds != NULL || submission.waiting != 0U) {
		result = hold_submit(session, device, &submission, fence);
		ghci_vk_reply_result(reply, requested, result);
		return 0;
	}

	/* The work and its fence in the device's session (the queue holds no lock of the device). */
	result = (uint32_t)VK_SUCCESS;
	error = ghci_cp_submit_placed(session->device, device->vm, submission.words, submission.used, &ticket, &placed);
	if (error != 0)
		result = (uint32_t)VK_ERROR_DEVICE_LOST;

	/*
	 * The ticket the fence, the jobs, the frames and the submission's
	 * memory wait for.  A submission placed in part before a recovery
	 * has a ticket too, which the reset ends: what it reads is kept
	 * until then, and it is the device's last work.  One that placed
	 * nothing gives its memory back at once.
	 */
	if (result == (uint32_t)VK_SUCCESS) {
		ghci_vk_submitted(device, &ticket);
		if (fence != NULL) {
			fence->as.fence.signaled = 0;
			fence->as.fence.submitted = 1;
			fence->as.fence.ticket = ticket;
		}
		ghci_vk_submission_end(device, &submission, &ticket, 1);
	} else if (placed) {
		ghci_vk_submitted(device, &ticket);
		ghci_vk_submission_end(device, &submission, &ticket, 1);
	} else {
		ghci_vk_submission_end(device, &submission, NULL, 0);
	}

	/* The result. */
	ghci_vk_reply_result(reply, requested, result);

	/* Succeeded: the work is in the ring, or the reply says the device is lost. */
	return 0;
}

/*
 * Checks the submissions: every array as long as its count, every
 * semaphore the device's, and every command buffer the device's, primary
 * and ended; *recorded says whether one has commands.  Returns 0, ENOMEM
 * for a command buffer whose recording was not kept (its End answered out
 * of host memory), or EINVAL for one the session does not have, one not
 * ready or a count that is not its array's.
 */
static int
submission_check(
	struct ghci_vk_session *session,
	const VkSubmitInfo *submits,
	uint64_t count,
	struct ghci_vk_device *device,
	int *recorded)
{
	struct ghci_vk_object *object;
	uint64_t decoded[4];
	uint64_t index;
	uint32_t at;
	uint64_t handle;

	/* Each submission's semaphores and command buffers. */
	*recorded = 0;
	for (index = 0U; index < count; index++) {
		/* Arrays as long as their counts say (an absent one is not read). */
		decoded[0] = ghci_vk_decoded_count(session, submits[index].pWaitSemaphores);
		decoded[1] = ghci_vk_decoded_count(session, submits[index].pWaitDstStageMask);
		decoded[2] = ghci_vk_decoded_count(session, submits[index].pCommandBuffers);
		decoded[3] = ghci_vk_decoded_count(session, submits[index].pSignalSemaphores);
		if (submits[index].pWaitSemaphores != NULL && decoded[0] != submits[index].waitSemaphoreCount)
			return EINVAL;
		if (submits[index].pWaitDstStageMask != NULL && decoded[1] != submits[index].waitSemaphoreCount)
			return EINVAL;
		if (submits[index].pCommandBuffers != NULL && decoded[2] != submits[index].commandBufferCount)
			return EINVAL;
		if (submits[index].pSignalSemaphores != NULL && decoded[3] != submits[index].signalSemaphoreCount)
			return EINVAL;

		/* The semaphores waited for and signaled. */
		for (at = 0U; at < submits[index].waitSemaphoreCount && submits[index].pWaitSemaphores != NULL; at++) {
			handle = (uint64_t)(uintptr_t)submits[index].pWaitSemaphores[at];
			object = ghci_vk_object_find(session, GHCI_VK_SEMAPHORE, handle);
			if (object == NULL || object->device != device)
				return EINVAL;
		}
		for (at = 0U; at < submits[index].signalSemaphoreCount && submits[index].pSignalSemaphores != NULL; at++) {
			handle = (uint64_t)(uintptr_t)submits[index].pSignalSemaphores[at];
			object = ghci_vk_object_find(session, GHCI_VK_SEMAPHORE, handle);
			if (object == NULL || object->device != device)
				return EINVAL;
		}

		/* The command buffers: primary and ended. */
		if (submits[index].commandBufferCount != 0U && submits[index].pCommandBuffers == NULL)
			return EINVAL;
		for (at = 0U; at < submits[index].commandBufferCount; at++) {
			handle = (uint64_t)(uintptr_t)submits[index].pCommandBuffers[at];
			object = ghci_vk_object_find(session, GHCI_VK_COMMAND_BUFFER, handle);
			if (object == NULL || object->device != device)
				return EINVAL;
			if (object->as.commands.state != GHCI_VK_RECORD_EXECUTABLE)
				return EINVAL;

			/* One whose recording was not kept is turned down with its End's result, out of host memory. */
			if (object->as.commands.failed)
				return ENOMEM;
			if (object->as.commands.level != (uint32_t)VK_COMMAND_BUFFER_LEVEL_PRIMARY)
				return EINVAL;
			if (object->as.commands.commands != 0U)
				*recorded = 1;
		}
	}

	/* Succeeded: the submissions may run. */
	return 0;
}

/*
 * vkCreateFence, vkCreateSemaphore and vkCreateEvent: the create form
 * with [sType][u64 0][u32 flags].  A fence may start signaled; an event
 * takes a word of the device's executor heap, cleared.
 */
static int
create_sync(
	struct ghci_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkFenceCreateInfo fence_info;
	VkSemaphoreCreateInfo semaphore_info;
	VkEventCreateInfo event_info;
	struct ghci_vk_object *device;
	struct ghci_vk_object *object;
	enum ghci_vk_kind kind;
	struct ghci_vk_heap_block *block;
	uint64_t handle;
	uint64_t present;
	uint64_t identity;
	uint32_t offset;
	uint32_t result;
	int error;

	/* The device and the create info of the kind. */
	kern_memset(&fence_info, 0, sizeof(fence_info));
	handle = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	kind = GHCI_VK_EVENT;
	if (opcode == GPU_OP_CREATE_FENCE) {
		kind = GHCI_VK_FENCE;
		i915_vkc_dec_VkFenceCreateInfo(reader, &session->arena, &fence_info);
	} else if (opcode == GPU_OP_CREATE_SEMAPHORE) {
		kind = GHCI_VK_SEMAPHORE;
		i915_vkc_dec_VkSemaphoreCreateInfo(reader, &session->arena, &semaphore_info);
	} else {
		i915_vkc_dec_VkEventCreateInfo(reader, &session->arena, &event_info);
	}
	identity = ghci_vk_create_tail(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;
	device = ghci_vk_object_find(session, GHCI_VK_DEVICE, handle);
	if (device == NULL)
		return EINVAL;

	/* An event's word comes first: a heap without room is the reply's. */
	result = (uint32_t)VK_SUCCESS;
	block = NULL;
	offset = 0U;
	if (kind == GHCI_VK_EVENT) {
		error = ghci_vk_heap_take(session, device->device, GHCI_VK_HEAP_DATA, EVENT_BYTES, EVENT_BYTES, &block, &offset);
		result = ghci_vk_result(error);
	}

	/* The object. */
	if (result == (uint32_t)VK_SUCCESS) {
		error = ghci_vk_object_add(session, kind, identity, device->device, &object);
		if (error != 0) {
			ghci_vk_heap_give(block, offset);
			return error;
		}
		if (kind == GHCI_VK_FENCE && (fence_info.flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0U)
			object->as.fence.signaled = 1;
		if (kind == GHCI_VK_EVENT) {
			object->as.event.block = block;
			object->as.event.offset = offset;
			object->as.event.word = (volatile uint64_t *)(void *)(block->allocation->view + offset);
			*object->as.event.word = 0U;
		}
	}

	/* The result, and the identity when it succeeded. */
	ghci_vk_reply_created(reply, requested, result, identity);

	/* Succeeded: the object exists, or the reply says why not. */
	return 0;
}

/* vkDestroyFence, vkDestroySemaphore and vkDestroyEvent: the destroy form, no reply body. */
static int
destroy_sync(
	struct ghci_vk_session *session,
	enum ghci_vk_kind kind,
	struct i915_wire_reader *reader)
{
	struct ghci_vk_object *object;
	uint64_t identity;
	uint64_t device;

	/* The destroy form. */
	identity = ghci_vk_destroy_form(reader, &device);
	if (reader->error != 0)
		return EINVAL;

	/* The object, when the session has it. */
	object = ghci_vk_object_find(session, kind, identity);
	if (object != NULL)
		ghci_vk_object_destroy(session, object);

	/* Succeeded: the object is gone. */
	return 0;
}

/* vkResetFences: [device][u32 count][u64 count][count x fence] -> [result]. */
static int
reset_fences(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct ghci_vk_object *object;
	const uint8_t *identities;
	uint64_t identity;
	uint64_t count;
	uint64_t index;

	/* The device and the fences. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = ghci_vk_read_count(reader);
	identities = drv_i915_wire_read_array(reader, (size_t)count, 8U);
	if (reader->error != 0)
		return EINVAL;

	/* Each fence of the session is unsignaled and waits for no submission. */
	for (index = 0U; index < count; index++) {
		kern_memcpy(&identity, identities + index * 8U, 8U);
		object = ghci_vk_object_find(session, GHCI_VK_FENCE, identity);
		if (object == NULL)
			return EINVAL;
		object->as.fence.signaled = 0;
		object->as.fence.submitted = 0;
		object->as.fence.held = 0;
		object->as.fence.lost = 0;
	}
	ghci_vk_reply_result(reply, requested, (uint32_t)VK_SUCCESS);

	/* Succeeded: the fences are reset. */
	return 0;
}

/*
 * vkGetFenceStatus: [device][fence] -> [result]: VK_SUCCESS signaled,
 * VK_NOT_READY not yet (or held back), VK_ERROR_DEVICE_LOST for work a
 * reset ended, held work that failed, or a fence of a lost device.  A fence of held work first lets
 * the device's held submissions go as far as they may (on the client's
 * thread, which may wait for the ring's room), so that a client that
 * polls its fence places what the service thread could not.
 */
static int
fence_status(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	enum ghci_ticket_state state;
	struct ghci_vk_object *object;
	uint64_t identity;
	uint32_t result;
	int lost;

	/* The device and the fence. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	object = ghci_vk_object_find(session, GHCI_VK_FENCE, identity);
	if (object == NULL)
		return EINVAL;

	/* Held work goes as far as it may now. */
	if (object->as.fence.held)
		(void)ghci_vk_holds_advance(session, object->device, 1);

	/* Signaled on the host, by its submission's ticket, or not yet; nothing on a lost device. */
	result = (uint32_t)VK_NOT_READY;
	lost = 0;
	if (object->device != NULL)
		lost = __atomic_load_n(&object->device->lost, __ATOMIC_ACQUIRE);
	if (object->as.fence.lost || lost) {
		result = (uint32_t)VK_ERROR_DEVICE_LOST;
	} else if (object->as.fence.signaled) {
		result = (uint32_t)VK_SUCCESS;
	} else if (object->as.fence.submitted) {
		state = ghci_cp_ticket(session->device, &object->as.fence.ticket);
		if (state == GHCI_TICKET_DONE) {
			object->as.fence.signaled = 1;
			result = (uint32_t)VK_SUCCESS;
		} else if (state == GHCI_TICKET_FAILED) {
			result = (uint32_t)VK_ERROR_DEVICE_LOST;
		}
	}
	ghci_vk_reply_result(reply, requested, result);

	/* Succeeded: the status is reported. */
	return 0;
}

/*
 * vkGetEventStatus, vkSetEvent and vkResetEvent: [device][event] ->
 * [result], on the event's word.  A set lets the device's held
 * submissions go as far as they may now.
 */
static int
event_operation(
	struct ghci_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct ghci_vk_object *object;
	uint64_t identity;
	uint32_t result;

	/* The device and the event. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	object = ghci_vk_object_find(session, GHCI_VK_EVENT, identity);
	if (object == NULL)
		return EINVAL;

	/* The word read, set or cleared. */
	result = (uint32_t)VK_SUCCESS;
	if (opcode == GPU_OP_SET_EVENT) {
		*object->as.event.word = 1U;
		(void)ghci_vk_holds_advance(session, object->device, 1);
	} else if (opcode == GPU_OP_RESET_EVENT) {
		*object->as.event.word = 0U;
	} else if (*object->as.event.word != 0U) {
		result = (uint32_t)VK_EVENT_SET;
	} else {
		result = (uint32_t)VK_EVENT_RESET;
	}
	ghci_vk_reply_result(reply, requested, result);

	/* Succeeded: the event is answered. */
	return 0;
}

/* Answers a marker's ticket from the device's fence and reset generations (irq_lock held). */
static enum ghci_job_answer
judge(
	void *context,
	uint64_t seq,
	uint64_t generation)
{
	enum ghci_ticket_state state;
	struct ghci_ticket ticket;

	/* Held work is not placed yet. */
	if (generation == GHCI_VK_HELD_GENERATION)
		return GHCI_JOB_WAITING;

	/* The ticket's state. */
	ticket.seq = seq;
	ticket.generation = generation;
	state = ghci_cp_ticket_locked(context, &ticket);

	/* Passed, ended by a reset, or not yet. */
	if (state == GHCI_TICKET_DONE)
		return GHCI_JOB_PASSED;
	if (state == GHCI_TICKET_FAILED)
		return GHCI_JOB_LOST;

	/* Succeeded: not yet. */
	return GHCI_JOB_WAITING;
}

/* Retires and reports, batch by batch, every completion of one session that is done; no lock is held. */
static void
retire_session(
	struct ghci_device *device,
	struct ghci_vk_session *session)
{
	struct ghci_vk_session *owners[GHCI_VK_DONE_BATCH];
	struct ghci_vk_frame *gone;
	void *done[GHCI_VK_DONE_BATCH];
	int errors[GHCI_VK_DONE_BATCH];
	unsigned long irq;
	unsigned count;

	/* Batches until a batch is not full. */
	do {
		gone = NULL;
		irq = spin_lock_irqsave(&device->irq_lock);

		count = retire(session, done, errors, owners, GHCI_VK_DONE_BATCH, &gone);

		spin_unlock_irqrestore(&device->irq_lock, irq);
		complete(device, done, errors, owners, count, gone);
	} while (count == GHCI_VK_DONE_BATCH);
}

/*
 * Takes a session's done markers and frames (irq_lock held): their
 * completions and errors to done and errors and the session to owners
 * (at most capacity), and the frames' records to gone, to be freed once
 * the lock is let go.  What is taken counts as the session's completing
 * until complete() has reported it.  Returns the number taken.
 */
static unsigned
retire(
	struct ghci_vk_session *session,
	void **done,
	int *errors,
	struct ghci_vk_session **owners,
	unsigned capacity,
	struct ghci_vk_frame **gone)
{
	enum ghci_ticket_state state;
	struct ghci_vk_frame **position;
	struct ghci_vk_frame *frame;
	uint64_t waiting;
	uint64_t mark;
	unsigned count;
	unsigned index;

	/* The markers. */
	count = ghci_jobs_retire(&session->jobs, judge, session->device, done, errors, capacity);

	/* The frames, in order within a timeline: one that waits keeps its timeline's later ones waiting. */
	waiting = 0U;
	position = &session->frames;
	while (count < capacity && *position != NULL) {
		frame = *position;
		mark = (uint64_t)1U << frame->timeline;
		state = GHCI_TICKET_DONE;
		if ((waiting & mark) != 0U)
			state = GHCI_TICKET_PENDING;
		else if (frame->failed)
			state = GHCI_TICKET_FAILED;
		else if (frame->ticket.generation == GHCI_VK_HELD_GENERATION)
			state = GHCI_TICKET_PENDING;
		else if (frame->ticket.seq != 0U)
			state = ghci_cp_ticket_locked(session->device, &frame->ticket);
		if (state == GHCI_TICKET_PENDING) {
			waiting |= mark;
			position = &frame->next;
			continue;
		}

		/* The frame leaves the queue with its answer. */
		*position = frame->next;
		session->frame_count--;
		done[count] = frame->completion;
		errors[count] = 0;
		if (state == GHCI_TICKET_FAILED)
			errors[count] = EIO;
		count++;
		frame->next = *gone;
		*gone = frame;
	}

	/* What was taken is the session's until it is reported. */
	for (index = 0U; index < count; index++)
		owners[index] = session;
	session->completing += count;

	/* Succeeded: the number taken. */
	return count;
}

/*
 * Completes what a retirement took and frees the frames' records; no lock
 * is held.  Each completion leaves its session's completing once the
 * core has heard of it, and the last one wakes a drain; the session is
 * not touched after that.
 */
static void
complete(
	struct ghci_device *device,
	void **done,
	const int *errors,
	struct ghci_vk_session **owners,
	unsigned count,
	struct ghci_vk_frame *gone)
{
	struct ghci_vk_frame *next;
	struct ghci_vk *vk;
	unsigned long irq;
	unsigned index;

	/* The core hears of each completion, and then its session counts it reported. */
	vk = device->vk;
	for (index = 0U; index < count; index++) {
		drv_gpu_complete(done[index], errors[index]);

		irq = spin_lock_irqsave(&device->irq_lock);

		owners[index]->completing--;
		if (owners[index]->completing == 0U)
			waitq_wake_all(&vk->completing_wake);

		spin_unlock_irqrestore(&device->irq_lock, irq);
	}

	/* The frames' records. */
	while (gone != NULL) {
		next = gone->next;
		kern_free(gone);
		gone = next;
	}
}

/*
 * Holds a submission back behind the device's held ones (or at its cut),
 * with the fence it signals, and places it as far as it may go now.  What
 * it keeps of the host's memory (its record, packets and cuts) and of the
 * device's data heap (its temporary memory) is taken of the session's
 * budget until it is placed or fails, so that a client cannot pile up
 * held work without bound.  Returns the reply: VK_SUCCESS (placed or
 * held), VK_ERROR_DEVICE_LOST when the held work failed, or
 * VK_ERROR_OUT_OF_HOST_MEMORY (the host memory) or
 * VK_ERROR_OUT_OF_DEVICE_MEMORY (the temporary memory) with nothing
 * held.
 */
static uint32_t
hold_submit(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device,
	struct ghci_vk_submission *submission,
	struct ghci_vk_object *fence)
{
	struct ghci_vk_hold **position;
	struct ghci_vk_hold *hold;
	unsigned long irq;
	uint64_t bytes;
	int error;

	/* The host bytes it keeps while held, of the session's budget. */
	bytes = sizeof(*hold) + (uint64_t)submission->capacity * 4U + (uint64_t)submission->cut_capacity * sizeof(struct ghci_vk_cut) +
		(uint64_t)submission->wait_capacity * 8U + (uint64_t)submission->temp_capacity * sizeof(struct ghci_vk_temp);
	error = ghci_vk_budget_take(session, bytes);
	if (error != 0) {
		ghci_vk_submission_end(device, submission, NULL, 0);
		return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Its temporary memory in the data heap, of the same budget. */
	error = ghci_vk_budget_take(session, submission->temp_bytes);
	if (error != 0) {
		ghci_vk_budget_give(session, bytes);
		ghci_vk_submission_end(device, submission, NULL, 0);
		return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	bytes += submission->temp_bytes;

	/* The record takes the submission over. */
	hold = kern_calloc(1U, sizeof(*hold));
	if (hold == NULL) {
		ghci_vk_budget_give(session, bytes);
		ghci_vk_submission_end(device, submission, NULL, 0);
		return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	hold->submission = *submission;
	hold->charged = bytes;
	kern_memset(submission, 0, sizeof(*submission));

	/* The fence waits for it, unsignaled. */
	if (fence != NULL) {
		hold->fence = fence->identity;
		fence->as.fence.signaled = 0;
		fence->as.fence.submitted = 0;
		fence->as.fence.held = 1;
		fence->as.fence.lost = 0;
	}

	/* Its serial and count, which the jobs and frames read. */
	irq = spin_lock_irqsave(&session->device->irq_lock);

	device->hold_serial++;
	hold->serial = device->hold_serial;
	device->hold_count++;
	session->hold_count++;

	spin_unlock_irqrestore(&session->device->irq_lock, irq);

	/* Last of the device's held submissions. */
	position = &device->holds;
	while (*position != NULL)
		position = &(*position)->next;
	*position = hold;

	/* As far as it may go now. */
	error = ghci_vk_holds_advance(session, device, 1);
	if (error != 0)
		return (uint32_t)VK_ERROR_DEVICE_LOST;

	/* Succeeded: placed, or held until its events are set. */
	return (uint32_t)VK_SUCCESS;
}

/*
 * Places the pieces of a held submission that may go now.  Returns 0
 * once it is placed whole, EAGAIN when a piece waits (for an event, or
 * for the ring), or the error that fails it.
 */
static int
hold_step(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device,
	struct ghci_vk_hold *hold,
	int wait)
{
	uint32_t end;
	int error;

	/* Piece after piece (one, with the fence alone, for a submission of no packets). */
	do {
		error = piece_end(session, hold, wait, &end);
		if (error != 0)
			return error;
		error = piece_place(session, device, hold, end, wait);
		if (error != 0)
			return error;
	} while (hold->placed < hold->submission.used);

	/* Succeeded: placed whole. */
	return 0;
}

/*
 * Reports where the next piece of a held submission ends: past every cut
 * whose events are set, up to the first whose are not; without wait, no
 * longer than the ring takes at once (half the ring, less a piece's own
 * dwords) unless one stretch between two cuts is.  A cut whose events
 * are set no longer waits.  Returns 0 with *end, EAGAIN when the piece
 * would be empty (the first cut waits), or EINVAL when an event it waits
 * for is gone.
 */
static int
piece_end(
	struct ghci_vk_session *session,
	struct ghci_vk_hold *hold,
	int wait,
	uint32_t *end)
{
	struct ghci_vk_submission *submission;
	uint32_t limit;
	uint32_t next;
	uint32_t cut;
	uint32_t at;
	int error;

	/* What the ring takes at once. */
	submission = &hold->submission;
	limit = session->device->ring.dwords / 2U;
	if (limit > PIECE_EXTRA_DWORDS)
		limit -= PIECE_EXTRA_DWORDS;
	else
		limit = 1U;

	/* From the first dword not placed, cut by cut. */
	at = hold->placed;
	cut = hold->cut;
	for (;;) {
		/* The cut here: its events set, or the piece ends before it. */
		if (cut < submission->cut_count && submission->cuts[cut].at == at) {
			if (submission->cuts[cut].count != 0U) {
				/* Its events: an empty piece waits, a longer one ends before the cut. */
				error = cut_ready(session, hold, &submission->cuts[cut]);
				if (error != 0 && at == hold->placed)
					return error;
				if (error != 0)
					break;

				/* Set: the cut no longer waits. */
				submission->cuts[cut].count = 0U;
			}
			cut++;
		}

		/* The end, or the next cut within what the ring takes at once. */
		if (at == submission->used)
			break;
		next = submission->used;
		if (cut < submission->cut_count)
			next = submission->cuts[cut].at;
		if (!wait &&
		    at != hold->placed &&
		    next - hold->placed > limit)
			break;
		at = next;
	}

	/* Succeeded: the piece's end. */
	*end = at;
	return 0;
}

/*
 * Reports whether the events of a cut of a held submission are set: no
 * placed work writes the event any more, and then its word is 1 (read
 * after the ticket, so that a word placed work set and then reset is not
 * seen at 1 between the two); an event the submission itself writes
 * between its first dword not placed and the cut is not set yet (the
 * piece ends at the cut, and its ticket is then waited for).  Returns 0,
 * EAGAIN when one is not yet, or EINVAL when one is gone.
 */
static int
cut_ready(
	struct ghci_vk_session *session,
	const struct ghci_vk_hold *hold,
	const struct ghci_vk_cut *cut)
{
	const struct ghci_vk_submission *submission;
	struct ghci_vk_object *object;
	enum ghci_ticket_state state;
	uint64_t identity;
	uint32_t index;
	uint32_t at;

	/* Each event of the cut. */
	submission = &hold->submission;
	for (index = 0U; index < cut->count; index++) {
		/* Written by the part of the submission not placed yet, before the cut. */
		identity = submission->waits[cut->first + index];
		for (at = 0U; at < submission->event_count && cut->at > hold->placed; at++) {
			if (submission->events[at] != identity)
				continue;
			if (submission->event_first[at] < cut->at && submission->event_last[at] >= hold->placed)
				return EAGAIN;
		}

		/* The placed work that writes it, which has finished. */
		object = ghci_vk_object_find(session, GHCI_VK_EVENT, identity);
		if (object == NULL)
			return EINVAL;
		if (object->as.event.gpu_written) {
			state = ghci_cp_ticket(session->device, &object->as.event.written);
			if (state == GHCI_TICKET_PENDING)
				return EAGAIN;
		}

		/* Then the word, as that work left it (or the host set it). */
		kern_io_read_barrier();
		if (*object->as.event.word != 1U)
			return EAGAIN;
	}

	/* Succeeded: every event is set. */
	return 0;
}

/*
 * Places a held submission's packets up to the dword end, after a
 * barrier that shows the GPU what the host wrote when it does not begin
 * the submission and before one that puts the GPU's writes in memory
 * when it does not end it.  The piece's ticket is the device's last and
 * the events it writes wait for it.  Returns 0, EAGAIN when the ring
 * does not take it now (without wait), or the error that fails it.
 */
static int
piece_place(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device,
	struct ghci_vk_hold *hold,
	uint32_t end,
	int wait)
{
	struct ghci_vk_submission *submission;
	const uint32_t *packets;
	struct ghci_ticket ticket;
	int placed;
	int error;

	/* The piece (none for a submission of no packets). */
	submission = &hold->submission;
	packets = NULL;
	if (end != hold->placed)
		packets = submission->words + hold->placed;
	error = ring_place(session->device, device->vm, packets, end - hold->placed, hold->placed != 0U, end != submission->used, wait,
		&ticket, &placed);

	/* What reached the ring has a ticket, which its memory and events wait for. */
	if (error == 0 || placed) {
		hold->ticket = ticket;
		hold->ticketed = 1;
		ghci_vk_submitted(device, &ticket);
		ghci_vk_submission_written(device, submission, end, &ticket);
	}
	if (error != 0)
		return error;

	/* The next piece starts at end, at the first cut there or after. */
	hold->placed = end;
	while (hold->cut < submission->cut_count && submission->cuts[hold->cut].at < end)
		hold->cut++;

	/* Succeeded: the piece is in the ring. */
	return 0;
}

/*
 * Places a run of packets in a GHCI session as ghci_cp_submit_placed()
 * does (the session's bind, the packets, the fence and session 0's
 * bind), with the head barrier before them and the tail barrier after
 * them when asked.  With wait it passes the gate and may wait for the
 * ring's room; without, it enters only a free ring with room for all of
 * it now.  Returns 0 with the ticket; EAGAIN (without wait) with nothing
 * placed; EIO for a lost device, a session that is not open, or a
 * recovery that began in the middle (*placed and the ticket then say
 * what is the reset's to end); or EINVAL for malformed packets.
 */
static int
ring_place(
	struct ghci_device *gpu,
	struct ghci_vm *vm,
	const uint32_t *packets,
	uint32_t dwords,
	int head,
	int tail,
	int wait,
	struct ghci_ticket *ticket,
	int *placed)
{
	enum ghci_gate gate;
	uint32_t bind[GHCI_DW_BIND_SESSION];
	uint32_t barrier[GHCI_DW_BARRIER];
	unsigned long irq;
	uint32_t start;
	uint32_t need;
	int walker;
	int fenced;
	int error;

	/* Packets the ring could not take are refused before anything is placed. */
	*placed = 0;
	error = ghci_cp_check_packets(packets, dwords);
	if (error != 0)
		return error;

	/* The gate: waited through, or only a free ring with the room now. */
	walker = ghci_caps_has(&gpu->caps, GHCI_HAS_HW_WALKER);
	if (wait) {
		for (;;) {
			gate = ghci_cp_enter(gpu, 0U, 1);
			if (gate == GHCI_GATE_LOST)
				return EIO;
			if (gate == GHCI_GATE_OPEN)
				break;
		}
	} else {
		need = dwords + GHCI_DW_FENCE;
		if (walker)
			need += 2U * GHCI_DW_BIND_SESSION;
		if (head)
			need += GHCI_DW_BARRIER;
		if (tail)
			need += GHCI_DW_BARRIER;
		gate = ghci_cp_try_enter(gpu, need);
		if (gate == GHCI_GATE_LOST)
			return EIO;
		if (gate != GHCI_GATE_OPEN)
			return EAGAIN;
	}

	/* Only an open session runs packets; its state does not change while cp_lock is held. */
	if (vm->state != GHCI_VM_OPEN) {
		ghci_cp_leave(gpu);
		return EIO;
	}

	/* The session, the head barrier, the packets, the tail barrier and the fence. */
	start = gpu->ring.wptr;
	fenced = 0;
	if (walker) {
		(void)ghci_cmd_bind_session(bind, vm->session);
		error = ghci_cp_place(gpu, bind, GHCI_DW_BIND_SESSION);
	}
	if (error == 0 && head) {
		(void)ghci_cmd_barrier(barrier, 0U, 0U, GHCI_VK_HEAD_FLAGS);
		error = ghci_cp_place(gpu, barrier, GHCI_DW_BARRIER);
	}
	if (error == 0)
		error = ghci_cp_place(gpu, packets, dwords);
	if (error == 0 && tail) {
		(void)ghci_cmd_barrier(barrier, 0U, 0U, GHCI_VK_TAIL_FLAGS);
		error = ghci_cp_place(gpu, barrier, GHCI_DW_BARRIER);
	}
	if (error == 0) {
		error = ghci_cp_place_fence(gpu, vm, start, ticket);
		if (error == 0)
			fenced = 1;
	}

	/* Session 0 again, so that the driver's later packets do not run in the client's session. */
	if (error == 0 && walker) {
		(void)ghci_cmd_bind_session(bind, 0U);
		error = ghci_cp_place(gpu, bind, GHCI_DW_BIND_SESSION);
	}

	/* A part placed without its fence: a ticket past every fence placed, which the reset that throws it away ends. */
	if (gpu->ring.wptr != start)
		*placed = 1;
	if (*placed && !fenced) {
		irq = spin_lock_irqsave(&gpu->irq_lock);

		ticket->seq = gpu->fence_placed + 1U;
		ticket->generation = gpu->generation;

		spin_unlock_irqrestore(&gpu->irq_lock, irq);
	}

	/* The doorbell for what was placed. */
	ghci_cp_doorbell(gpu);
	ghci_cp_leave(gpu);

	/* A recovery that began in the middle lost the piece. */
	if (error == EAGAIN)
		return EIO;
	if (error != 0)
		return error;

	/* Succeeded: the piece is in the ring. */
	return 0;
}

/*
 * Ends a held submission placed whole: the job markers and frames that
 * wait for it take its last ticket, its VkFence waits for that ticket,
 * and its memory goes back once the ticket has passed.  The caller holds
 * the session's hold_lock.
 */
static void
hold_finish(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device,
	struct ghci_vk_hold *hold)
{
	struct ghci_vk_object *object;
	unsigned long irq;

	/* It is no longer held; what waited for it waits for its ticket. */
	irq = spin_lock_irqsave(&session->device->irq_lock);

	device->hold_count--;
	session->hold_count--;
	held_resolve(session, device, hold->serial, &hold->ticket);

	spin_unlock_irqrestore(&session->device->irq_lock, irq);

	/* The fence it signals, still waiting for it (none for identity 0; a reset of the fence lets it go). */
	object = ghci_vk_object_find(session, GHCI_VK_FENCE, hold->fence);
	if (object != NULL && object->as.fence.held) {
		object->as.fence.held = 0;
		object->as.fence.submitted = 1;
		object->as.fence.ticket = hold->ticket;
	}

	/* Its memory, after its last piece, and the budget it took. */
	ghci_vk_submission_end(device, &hold->submission, &hold->ticket, 1);
	ghci_vk_budget_give(session, hold->charged);
	kern_free(hold);
}

/*
 * Reports whether a device's held work can no longer run: its session
 * stopped or was isolated, the GPU is lost, or a reset ended the work it
 * placed last (its GHCI session died with it).  Returns 1 when so.
 */
static int
holds_dead(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device)
{
	enum ghci_ticket_state state;
	unsigned long irq;
	int dead;

	/* The marks, the GPU and the device's last ticket, under the interrupt's lock. */
	irq = spin_lock_irqsave(&session->device->irq_lock);

	state = GHCI_TICKET_DONE;
	if (device->submitted)
		state = ghci_cp_ticket_locked(session->device, &device->last);
	dead = 0;
	if (session->stopping || session->isolated) {
		/* The GPU core stopped or isolated the session. */
		dead = 1;
	} else if (session->device->state == GHCI_STATE_LOST) {
		/* The GPU is lost. */
		dead = 1;
	} else if (state == GHCI_TICKET_FAILED) {
		/* A reset ended the device's placed work, and its GHCI session with it. */
		dead = 1;
	}

	spin_unlock_irqrestore(&session->device->irq_lock, irq);

	/* Succeeded: the answer. */
	return dead;
}

/*
 * Gives the job markers and frames that wait for a device's held
 * submissions (the device they were committed or queued behind) up to
 * serial the ticket of that submission, or, with no ticket, fails every
 * one of them; a timeline whose last frame so failed has reported the
 * failure (its bit in failed_timelines goes).  The caller holds
 * irq_lock.
 */
static void
held_resolve(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device,
	uint64_t serial,
	const struct ghci_ticket *ticket)
{
	struct ghci_job_marker *marker;
	struct ghci_vk_frame *frame;
	unsigned index;

	/* The markers committed behind held work of the device. */
	for (index = 0U; index < GHCI_JOB_MARKERS; index++) {
		marker = &session->jobs.markers[index];
		if (marker->state != GHCI_JOB_COMMITTED || marker->generation != GHCI_VK_HELD_GENERATION)
			continue;
		if (session->marker_device[index] != device)
			continue;
		if (ticket == NULL) {
			marker->state = GHCI_JOB_FAILED;
			session->marker_device[index] = NULL;
		} else if (marker->seq <= serial) {
			marker->seq = ticket->seq;
			marker->generation = ticket->generation;
			session->marker_device[index] = NULL;
		}
	}

	/* The frames queued behind it. */
	for (frame = session->frames; frame != NULL; frame = frame->next) {
		if (frame->ticket.generation != GHCI_VK_HELD_GENERATION || frame->held != device)
			continue;
		if (ticket == NULL) {
			/* Failed; the last held submission before it is the device's last: it reports every failure. */
			frame->failed = 1;
			frame->held = NULL;
			if (frame->ticket.seq == device->hold_serial)
				device->failed_timelines &= ~((uint64_t)1U << frame->timeline);
		} else if (frame->ticket.seq <= serial) {
			frame->ticket = *ticket;
			frame->held = NULL;
		}
	}
}

/*
 * Lets the held submissions of every node session go as far as they may
 * on the service thread: a session whose commands hold its hold_lock is
 * left for the next round, and nothing here waits for the ring.  Each
 * session looked at counts as holding until the look ends, so that its
 * drain waits.
 */
static void
holds_service(
	struct ghci_device *gpu)
{
	struct ghci_vk_session *sessions[GHCI_VK_DONE_BATCH];
	struct ghci_vk_session *session;
	struct ghci_vk_device *each;
	struct ghci_vk *vk;
	unsigned long irq;
	unsigned count;
	unsigned index;
	int locked;

	/* The sessions with held work. */
	vk = gpu->vk;
	count = 0U;
	irq = spin_lock_irqsave(&gpu->irq_lock);

	for (session = vk->sessions; session != NULL && count < GHCI_VK_DONE_BATCH; session = session->next) {
		if (session->hold_count == 0U)
			continue;
		session->holding++;
		sessions[count] = session;
		count++;
	}

	spin_unlock_irqrestore(&gpu->irq_lock, irq);

	/* Each one whose commands do not hold it now. */
	for (index = 0U; index < count; index++) {
		session = sessions[index];
		locked = mutex_trylock(&session->hold_lock);
		if (locked) {
			for (each = session->devices; each != NULL; each = each->next)
				(void)ghci_vk_holds_advance(session, each, 0);
			mutex_unlock(&session->hold_lock);
		}

		/* The look ends; the last one wakes a drain. */
		irq = spin_lock_irqsave(&gpu->irq_lock);

		session->holding--;
		if (session->holding == 0U)
			waitq_wake_all(&vk->completing_wake);

		spin_unlock_irqrestore(&gpu->irq_lock, irq);
	}
}

/*
 * Runs a query pool command; *handled stays clear for another group's
 * number.
 */
int
ghci_vk_query_route(
	struct ghci_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another group. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_QUERY_POOL:
		error = create_query_pool(session, requested, reader, reply);
		break;
	case GPU_OP_DESTROY_QUERY_POOL:
		error = destroy_sync(session, GHCI_VK_QUERY_POOL, reader);
		break;
	case GPU_OP_GET_QUERY_POOL_RESULTS:
		error = query_results(session, requested, reader, reply);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* A command that could not be decoded or run. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran. */
	return 0;
}

/*
 * vkCreateQueryPool: the create form with VkQueryPoolCreateInfo.  An
 * occlusion or timestamp pool takes 16 bytes of the device's data heap
 * per query, cleared (no query is available); a pipeline statistics pool
 * is answered VK_ERROR_FEATURE_NOT_PRESENT (the feature is not
 * reported), and a pool past QUERY_POOL_MAX queries or the heap
 * VK_ERROR_OUT_OF_DEVICE_MEMORY.
 */
static int
create_query_pool(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkQueryPoolCreateInfo info;
	struct ghci_vk_object *device;
	struct ghci_vk_object *object;
	struct ghci_vk_heap_block *block;
	uint64_t handle;
	uint64_t present;
	uint64_t identity;
	uint32_t offset;
	uint32_t bytes;
	uint32_t result;
	int error;

	/* The device, the create info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	handle = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present == 1U)
		i915_vkc_dec_VkQueryPoolCreateInfo(reader, &session->arena, &info);
	identity = ghci_vk_create_tail(reader);
	if (reader->error != 0 || present != 1U)
		return EINVAL;
	device = ghci_vk_object_find(session, GHCI_VK_DEVICE, handle);
	if (device == NULL)
		return EINVAL;

	/* The types the device counts, and the slots in the data heap. */
	result = (uint32_t)VK_SUCCESS;
	block = NULL;
	offset = 0U;
	bytes = 0U;
	if (info.queryType != VK_QUERY_TYPE_OCCLUSION && info.queryType != VK_QUERY_TYPE_TIMESTAMP)
		result = (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT;
	else if (info.queryCount == 0U || info.queryCount > QUERY_POOL_MAX)
		result = (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;
	if (result == (uint32_t)VK_SUCCESS) {
		bytes = info.queryCount * QUERY_SLOT_BYTES;
		error = ghci_vk_heap_take(session, device->device, GHCI_VK_HEAP_DATA, bytes, QUERY_SLOT_BYTES, &block, &offset);
		result = ghci_vk_result(error);
	}

	/* The object, its slots cleared. */
	if (result == (uint32_t)VK_SUCCESS) {
		error = ghci_vk_object_add(session, GHCI_VK_QUERY_POOL, identity, device->device, &object);
		if (error != 0) {
			ghci_vk_heap_give(block, offset);
			return error;
		}
		object->as.query_pool.type = info.queryType;
		object->as.query_pool.count = info.queryCount;
		object->as.query_pool.block = block;
		object->as.query_pool.offset = offset;
		object->as.query_pool.va = block->mapping->va + offset;
		object->as.query_pool.bus = block->allocation->bus + offset;
		object->as.query_pool.view = block->allocation->view + offset;
		kern_memset(object->as.query_pool.view, 0, bytes);
	}

	/* The result, and the identity when it succeeded. */
	ghci_vk_reply_created(reply, requested, result, identity);

	/* Succeeded: the pool exists, or the reply says why not. */
	return 0;
}

/*
 * vkGetQueryPoolResults: [device][pool][u32 first][u32 count][u64 bytes]
 * [u64 bytes][u64 stride][u32 flags] -> [result][u64 bytes][the records].
 * Each record is the query's result and its availability word, each
 * 4 bytes (8 with VK_QUERY_RESULT_64_BIT), packed at stride; the result is
 * VK_SUCCESS when every query is available and VK_NOT_READY when one is
 * not (libvulkan waits itself).  A shape that is not that is EINVAL.
 */
static int
query_results(
	struct ghci_vk_session *session,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct ghci_vk_object *object;
	const uint8_t *slot;
	uint64_t identity;
	uint64_t bytes;
	uint64_t stride;
	uint64_t value;
	uint64_t available;
	uint32_t first;
	uint32_t count;
	uint32_t flags;
	uint32_t width;
	uint32_t index;
	uint32_t result;

	/* The device, the pool, the queries and the shape. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	first = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	bytes = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	stride = drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;
	object = ghci_vk_object_find(session, GHCI_VK_QUERY_POOL, identity);
	if (object == NULL || first > object->as.query_pool.count || count > object->as.query_pool.count - first)
		return EINVAL;
	width = 4U;
	if ((flags & VK_QUERY_RESULT_64_BIT) != 0U)
		width = 8U;
	if (stride != 2U * width || bytes != (uint64_t)count * stride)
		return EINVAL;

	/* Every query available, or not yet. */
	result = (uint32_t)VK_SUCCESS;
	for (index = 0U; index < count; index++) {
		slot = object->as.query_pool.view + (uint64_t)(first + index) * QUERY_SLOT_BYTES;
		kern_memcpy(&available, slot + 8U, 8U);
		if (available == 0U)
			result = (uint32_t)VK_NOT_READY;
	}
	if (requested == 0U)
		return 0;

	/* The result, then each query's record. */
	drv_i915_wire_reply_u32(reply, result);
	drv_i915_wire_reply_u64(reply, bytes);
	for (index = 0U; index < count; index++) {
		slot = object->as.query_pool.view + (uint64_t)(first + index) * QUERY_SLOT_BYTES;
		kern_memcpy(&available, slot + 8U, 8U);

		/* The GPU writes the result before the availability word: the result is read after it. */
		kern_io_read_barrier();

		kern_memcpy(&value, slot, 8U);
		if (available != 0U)
			available = 1U;
		if (width == 8U) {
			drv_i915_wire_reply_u64(reply, value);
			drv_i915_wire_reply_u64(reply, available);
		} else {
			drv_i915_wire_reply_u32(reply, (uint32_t)value);
			drv_i915_wire_reply_u32(reply, (uint32_t)available);
		}
	}

	/* Succeeded: the records are reported. */
	return 0;
}
