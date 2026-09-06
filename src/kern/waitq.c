/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Wait queues.
 *
 * A wait queue pairs a sequence number with a list of sleeping threads.  A
 * sleeper passes the sequence it observed under the caller's condition lock,
 * so a wakeup that races the decision to sleep is never lost.  Interruptible
 * sleepers re-check signals, stop, cancellation, and termination requests on
 * both sides of the scheduler handoff.
 */

#include <kern/waitq.h>
#include <kern/atomic.h>
#include <kern/lock.h>
#include <kern/process.h>
#include <kern/sched.h>
#include <kern/signal.h>
#include <kern/thread.h>
#include <kern/test-checkpoint.h>

#include <errno.h>

static void waitq_remove(struct wait_queue *queue, struct wait_token *token);

/*
 * Initializes an empty wait queue.
 */
void
waitq_init(
	struct wait_queue *queue,
	const char *name)
{
	/* Starts empty at the first valid sequence. */
	queue->head = NULL;
	queue->tail = NULL;
	queue->sequence = 1;
	queue->name = name;
}

/*
 * Reads the current wakeup sequence of a wait queue.
 */
uint64_t
waitq_sequence(
	const struct wait_queue *queue)
{
	uint64_t sequence;

	/* Reads the sequence with acquire ordering against wakeups. */
	sequence = atomic_u64_load_acquire(&queue->sequence);

	/* Reports the observed sequence. */
	return sequence;
}

/*
 * Sleeps on a wait queue until it is woken, interrupted, or timed out.
 *
 * The caller holds condition_lock and passes the sequence it observed; the
 * sleep is refused with EAGAIN when a wakeup already advanced it.  With
 * WAITQ_INTERRUPTIBLE a pending signal, stop, cancellation, or termination
 * request ends the sleep with EINTR.  A zero deadline waits indefinitely.
 */
int
waitq_sleep(
	struct wait_queue *queue,
	struct spinlock *condition_lock,
	uint64_t observed,
	uint64_t deadline,
	unsigned flags)
{
	struct thread *thread;
	struct wait_token *token;
	uint64_t interrupt_generation;
	int pending;

	thread = thread_current();
	interrupt_generation = 0;

	/* Rejects a malformed request or an unknown flag combination. */
	if (queue == NULL ||
	    condition_lock == NULL ||
	    thread == NULL ||
	    (flags & ~(WAITQ_INTERRUPTIBLE | WAITQ_CANCELABLE)) != 0 ||
	    ((flags & WAITQ_CANCELABLE) != 0 &&
	     (flags & WAITQ_INTERRUPTIBLE) == 0))
		return EINVAL;

	/* Refuses to sleep past a wakeup that already happened. */
	if (waitq_sequence(queue) != observed)
		return EAGAIN;

	/* Surfaces any interruption that is already pending. */
	if ((flags & WAITQ_INTERRUPTIBLE) != 0) {
		/*
		 * Snapshot before inspecting pending state.  An interrupt
		 * delivered after this point is caught by the scheduler handoff
		 * even if its signal is consumed by another process thread before
		 * we inspect it.
		 */
		interrupt_generation = atomic_u64_load_acquire(
			&thread->interrupt_generation);
		spin_unlock(condition_lock);
		pending = signal_pending_unblocked(thread);
		spin_lock(condition_lock);

		/*
		 * A cancellation request advances interrupt_generation before
		 * waking the target.  Atomic pending state handles requests before
		 * this check; the scheduler generation handles the check-to-sleep
		 * handoff.
		 */
		if ((flags & WAITQ_CANCELABLE) != 0 &&
		    atomic_raw_load_acquire(&thread->cancel_pending) != 0)
			return EINTR;
		if (thread->terminate_requested)
			return EINTR;
		if (process_stop_requested(thread)) {
			thread->stop_interrupted = 1;
			return EINTR;
		}
		if (pending)
			return EINTR;

		/* Refuses to sleep past a wakeup delivered while unlocked. */
		if (waitq_sequence(queue) != observed)
			return EAGAIN;
	}

	KERN_TEST_CHECKPOINT(KERN_TEST_WAIT_BEFORE_REGISTER, queue);

	/* Rejects a thread that is already registered on a queue. */
	token = &thread->wait_token;
	if (token->queue != NULL)
		return EBUSY;

	/* Appends the thread's token to the queue. */
	token->thread = thread;
	token->next = NULL;
	token->queue = queue;
	if (queue->tail != NULL)
		queue->tail->next = token;
	else
		queue->head = token;
	queue->tail = token;

	KERN_TEST_CHECKPOINT(KERN_TEST_WAIT_AFTER_REGISTER, queue);

	/*
	 * Sleeps under the condition lock.  The wakeup reason is re-derived
	 * from the thread and queue state below rather than from the
	 * scheduler's result.
	 */
	if ((flags & WAITQ_INTERRUPTIBLE) != 0)
		(void)sched_sleep_locked_interruptible(
			deadline,
			condition_lock,
			interrupt_generation);
	else
		sched_sleep_locked(deadline, condition_lock);

	/* Unregisters a token that no wakeup removed. */
	if (token->queue == queue)
		waitq_remove(queue, token);

	/*
	 * exec and process exit use an out-of-band retirement request.  Every
	 * interruptible wait must surface it even when no ordinary signal is
	 * pending, otherwise a target can register itself again forever.
	 */
	if ((flags & WAITQ_INTERRUPTIBLE) != 0 &&
	    thread->terminate_requested)
		return EINTR;
	if ((flags & WAITQ_CANCELABLE) != 0 &&
	    atomic_raw_load_acquire(&thread->cancel_pending) != 0)
		return EINTR;
	if ((flags & WAITQ_INTERRUPTIBLE) != 0 &&
	    process_stop_requested(thread)) {
		thread->stop_interrupted = 1;
		return EINTR;
	}

	/* Reports a wakeup that advanced the sequence. */
	if (waitq_sequence(queue) != observed)
		return 0;

	/* Surfaces a signal that arrived during the sleep. */
	if ((flags & WAITQ_INTERRUPTIBLE) != 0) {
		/*
		 * The caller's condition lock can have any rank, including the
		 * process lock itself.  Signal inspection takes process->lock, so
		 * it must never run while the condition lock is held.  The
		 * wait-queue sequence closes the interval while the condition lock
		 * is dropped.
		 */
		spin_unlock(condition_lock);
		pending = signal_pending_unblocked(thread);
		spin_lock(condition_lock);
		if (pending)
			return EINTR;

		/* Reports a wakeup delivered while unlocked. */
		if (waitq_sequence(queue) != observed)
			return 0;
	}

	/* Reports an expired deadline. */
	if (deadline != 0 && sched_ticks() >= deadline)
		return ETIMEDOUT;

	/* Reports a spurious wakeup for the caller to re-check its condition. */
	return 0;
}

/*
 * Wakes the longest-waiting thread on a wait queue.
 *
 * The sequence advances even when no thread is sleeping, so a sleeper that
 * observed the old sequence refuses to sleep.
 */
void
waitq_wake_one(
	struct wait_queue *queue)
{
	struct wait_token *token;

	/* Ignores a missing queue. */
	if (queue == NULL)
		return;

	/* Publishes the wakeup before touching any sleeper. */
	(void)atomic_u64_fetch_add_relaxed(&queue->sequence, 1U);

	/* Returns when nobody is sleeping. */
	token = queue->head;
	if (token == NULL)
		return;

	/* Unregisters and wakes the head sleeper. */
	waitq_remove(queue, token);
	sched_wakeup(token->thread);
}

/*
 * Wakes every thread sleeping on a wait queue.
 */
void
waitq_wake_all(
	struct wait_queue *queue)
{
	struct wait_token *token;

	/* Ignores a missing queue. */
	if (queue == NULL)
		return;

	/* Publishes the wakeup before touching any sleeper. */
	(void)atomic_u64_fetch_add_relaxed(&queue->sequence, 1U);

	/* Unregisters and wakes every sleeper in queue order. */
	while (queue->head != NULL) {
		token = queue->head;
		waitq_remove(queue, token);
		sched_wakeup(token->thread);
	}
}

/* Unlinks one token from a wait queue and detaches it from the queue. */
static void
waitq_remove(
	struct wait_queue *queue,
	struct wait_token *token)
{
	struct wait_token **link;
	struct wait_token *previous;

	previous = NULL;

	/* Finds the link that points at the token and splices it out. */
	for (link = &queue->head; *link != NULL; link = &(*link)->next) {
		if (*link == token) {
			*link = token->next;
			if (queue->tail == token)
				queue->tail = previous;
			token->next = NULL;
			token->queue = NULL;
			return;
		}
		previous = *link;
	}
}
