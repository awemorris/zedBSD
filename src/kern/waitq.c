/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/waitq.h>
#include <kern/atomic.h>
#include <kern/lock.h>
#include <kern/process.h>
#include <kern/sched.h>
#include <kern/signal.h>
#include <kern/thread.h>
#include <kern/test-checkpoint.h>

#include <errno.h>

void waitq_init(struct wait_queue *queue, const char *name)
{ queue->head = queue->tail = NULL; queue->sequence = 1; queue->name = name; }
uint64_t waitq_sequence(const struct wait_queue *queue)
{ return atomic_u64_load_acquire(&queue->sequence); }

static void waitq_remove(struct wait_queue *queue, struct wait_token *token)
{
	struct wait_token **link, *previous = NULL;
	for (link = &queue->head; *link != NULL; link = &(*link)->next) {
		if (*link == token) {
			*link = token->next;
			if (queue->tail == token) queue->tail = previous;
			token->next = NULL; token->queue = NULL;
			return;
		}
		previous = *link;
	}
}

int waitq_sleep(struct wait_queue *queue, struct spinlock *condition_lock,
	uint64_t observed, uint64_t deadline, unsigned flags)
{
	struct thread *thread = thread_current();
	struct wait_token *token;
	uint64_t interrupt_generation = 0;
	int pending, interrupted = 0;
	if (queue == NULL || condition_lock == NULL || thread == NULL ||
	    (flags & ~(WAITQ_INTERRUPTIBLE | WAITQ_CANCELABLE)) != 0 ||
	    ((flags & WAITQ_CANCELABLE) != 0 &&
	    (flags & WAITQ_INTERRUPTIBLE) == 0)) return EINVAL;
	if (waitq_sequence(queue) != observed) return EAGAIN;
	if ((flags & WAITQ_INTERRUPTIBLE) != 0) {
		/* Snapshot before inspecting pending state.  An interrupt delivered
		 * after this point is caught by the scheduler handoff even if its signal
		 * is consumed by another process thread before we inspect it. */
		interrupt_generation = atomic_u64_load_acquire(
		    &thread->interrupt_generation);
		spin_unlock(condition_lock);
		pending = signal_pending_unblocked(thread);
		spin_lock(condition_lock);
		/* A cancellation request advances interrupt_generation before waking the
		 * target.  Atomic pending state handles requests before this check; the
		 * scheduler generation handles the check-to-sleep handoff. */
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
		if (waitq_sequence(queue) != observed)
			return EAGAIN;
	}
	KERN_TEST_CHECKPOINT(KERN_TEST_WAIT_BEFORE_REGISTER, queue);
	token = &thread->wait_token;
	if (token->queue != NULL) return EBUSY;
	token->thread = thread; token->next = NULL; token->queue = queue;
	if (queue->tail != NULL) queue->tail->next = token; else queue->head = token;
	queue->tail = token;
	KERN_TEST_CHECKPOINT(KERN_TEST_WAIT_AFTER_REGISTER, queue);
	if ((flags & WAITQ_INTERRUPTIBLE) != 0)
		interrupted = sched_sleep_locked_interruptible(deadline,
		    condition_lock, interrupt_generation);
	else
		sched_sleep_locked(deadline, condition_lock);
	if (token->queue == queue) waitq_remove(queue, token);
	/* exec and process exit use an out-of-band retirement request.  Every
	 * interruptible wait must surface it even when no ordinary signal is
	 * pending, otherwise a target can register itself again forever. */
	if ((flags & WAITQ_INTERRUPTIBLE) != 0 &&
	    thread->terminate_requested) return EINTR;
	if ((flags & WAITQ_CANCELABLE) != 0 &&
	    atomic_raw_load_acquire(&thread->cancel_pending) != 0)
		return EINTR;
	if ((flags & WAITQ_INTERRUPTIBLE) != 0 &&
	    process_stop_requested(thread)) {
		thread->stop_interrupted = 1;
		return EINTR;
	}
	if (waitq_sequence(queue) != observed)
		return 0;
	if ((flags & WAITQ_INTERRUPTIBLE) != 0) {
		/* The caller's condition lock can have any rank, including the
		 * process lock itself.  Signal inspection takes process->lock, so it
		 * must never run while the condition lock is held.  The wait-queue
		 * sequence closes the interval while the condition lock is dropped. */
		spin_unlock(condition_lock);
		pending = signal_pending_unblocked(thread);
		spin_lock(condition_lock);
		if (pending)
			return EINTR;
		if (waitq_sequence(queue) != observed)
			return 0;
	}
	(void)interrupted;
	if (deadline != 0 && sched_ticks() >= deadline) return ETIMEDOUT;
	return 0;
}

void waitq_wake_one(struct wait_queue *queue)
{
	struct wait_token *token;
	if (queue == NULL) return;
	(void)atomic_u64_fetch_add_relaxed(&queue->sequence, 1U);
	token = queue->head;
	if (token == NULL) return;
	waitq_remove(queue, token);
	sched_wakeup(token->thread);
}
void waitq_wake_all(struct wait_queue *queue)
{
	if (queue == NULL) return;
	(void)atomic_u64_fetch_add_relaxed(&queue->sequence, 1U);
	while (queue->head != NULL) {
		struct wait_token *token = queue->head;
		waitq_remove(queue, token);
		sched_wakeup(token->thread);
	}
}
