/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/waitq.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/signal.h>
#include <kern/thread.h>

#include <errno.h>

void waitq_init(struct wait_queue *queue, const char *name)
{ queue->head = queue->tail = NULL; queue->sequence = 1; queue->name = name; }
uint64_t waitq_sequence(const struct wait_queue *queue)
{ return __atomic_load_n(&queue->sequence, __ATOMIC_ACQUIRE); }

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
	if (queue == NULL || condition_lock == NULL || thread == NULL ||
	    (flags & ~WAITQ_INTERRUPTIBLE) != 0) return EINVAL;
	if (waitq_sequence(queue) != observed) return EAGAIN;
	token = &thread->wait_token;
	if (token->queue != NULL) return EBUSY;
	token->thread = thread; token->next = NULL; token->queue = queue;
	if (queue->tail != NULL) queue->tail->next = token; else queue->head = token;
	queue->tail = token;
	sched_sleep_locked(deadline, condition_lock);
	if (token->queue == queue) waitq_remove(queue, token);
	if ((flags & WAITQ_INTERRUPTIBLE) != 0 &&
	    signal_pending_unblocked(thread)) return EINTR;
	if (deadline != 0 && sched_ticks() >= deadline) return ETIMEDOUT;
	return 0;
}

void waitq_wake_one(struct wait_queue *queue)
{
	struct wait_token *token;
	if (queue == NULL) return;
	__atomic_add_fetch(&queue->sequence, 1U, __ATOMIC_RELEASE);
	token = queue->head;
	if (token == NULL) return;
	waitq_remove(queue, token);
	sched_wakeup(token->thread);
}
void waitq_wake_all(struct wait_queue *queue)
{
	if (queue == NULL) return;
	__atomic_add_fetch(&queue->sequence, 1U, __ATOMIC_RELEASE);
	while (queue->head != NULL) {
		struct wait_token *token = queue->head;
		waitq_remove(queue, token);
		sched_wakeup(token->thread);
	}
}
