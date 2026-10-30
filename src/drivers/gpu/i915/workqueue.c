/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Deferred work and delayed work (see workqueue.h).
 *
 * The work queue lock is taken with interrupts disabled because an
 * interrupt handler may queue work.  The timer queue lock is only taken
 * from threads, but with interrupts disabled as well: the kernel's spinlock
 * does not hold off preemption, and a holder preempted by the clock tick
 * may resume on another CPU, whose release traps as a release by a CPU
 * that does not own the lock.  The timer queue lock is held while the work
 * queue lock is taken only for the pending check of a queue request.
 */

#include "workqueue.h"

#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/waitq.h>

#include <stddef.h>

/* How many milliseconds make one second, for the delay conversion. */
#define I915_MILLISECONDS_PER_SECOND	1000U

static void i915_workqueue_worker(void *argument);
static int i915_workqueue_remove(struct i915_workqueue *queue, struct i915_work *work);
static void i915_timer_thread(void *argument);
static struct i915_delayed_work *i915_timer_take_due(struct i915_timer_queue *timers, uint64_t now, uint64_t *earliest);
static void i915_delayed_wait_not_firing(struct i915_timer_queue *timers, struct i915_delayed_work *delayed);
static int i915_delayed_disarm(struct i915_timer_queue *timers, struct i915_delayed_work *delayed);

/*
 * Prepares an idle work that runs a callback with an argument.
 */
void
drv_i915_work_init(
	struct i915_work *work,
	void (*function)(void *),
	void *context)
{
	/* Binds the callback and its argument. */
	work->function = function;
	work->context = context;

	/* Starts idle, on no queue, never run. */
	work->state = I915_WORK_IDLE;
	work->queued = 0;
	work->ran_count = 0;
	work->queue = NULL;

	/* Prepares the queue a synchronous cancel or a flush waits on. */
	waitq_init(&work->done_waitq, "i915-work-done");
}

/*
 * Creates a work queue and starts its worker thread.
 *
 * Returns 0, or the error of the thread creation; a failed queue has no
 * worker and needs no destroy.
 */
int
drv_i915_workqueue_create(
	struct i915_workqueue *queue,
	const char *name)
{
	struct thread *thread;
	unsigned slot;
	int error;

	/* Prepares the lock and the queue the worker sleeps on. */
	spin_init(&queue->lock, LOCK_RANK_DEVICE, name);
	waitq_init(&queue->waitq, name);

	/* Starts with an empty ring. */
	for (slot = 0U; slot < I915_WORKQUEUE_DEPTH; slot++)
		queue->slots[slot] = NULL;
	queue->head = 0U;
	queue->tail = 0U;
	queue->count = 0U;

	/*
	 * The worker counts as alive from here on, so a destroy that follows
	 * a successful create always waits for it to leave.
	 */
	queue->stop = 0;
	queue->worker_alive = 1;
	queue->worker = NULL;
	queue->name = name;

	/* Creates the worker that drains the queue. */
	error = kthread_create(i915_workqueue_worker, queue, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		queue->worker_alive = 0;
		return error;
	}

	/* The worker reclaims itself when it leaves. */
	queue->worker = thread;
	thread->detached = 1U;
	thread_start(thread);

	/* Succeeded: the worker runs and waits for work. */
	return 0;
}

/*
 * Stops a work queue's worker and waits until it has left.
 *
 * The worker first runs every work still queued; nothing queued afterwards
 * runs.
 */
void
drv_i915_workqueue_destroy(
	struct i915_workqueue *queue)
{
	unsigned long enabled;
	uint64_t observed;

	/* Asks the worker to leave and waits until it has. */
	enabled = spin_lock_irqsave(&queue->lock);

	queue->stop = 1;
	waitq_wake_all(&queue->waitq);

	/* Sleeps until the worker reports that it has left its loop. */
	while (queue->worker_alive != 0) {
		observed = waitq_sequence(&queue->waitq);
		(void)waitq_sleep(&queue->waitq, &queue->lock, observed, 0U, 0U);
	}

	spin_unlock_irqrestore(&queue->lock, enabled);
}

/*
 * Queues a work for the worker.
 *
 * Returns 1 when the work was newly queued, or 0 when it was already
 * pending or the queue is full.  A running work is not pending, so it may
 * be queued again and runs once more.  May be called from an interrupt
 * handler.
 */
int
drv_i915_queue_work(
	struct i915_workqueue *queue,
	struct i915_work *work)
{
	unsigned long enabled;

	/* Appends the work to the ring and wakes the worker. */
	enabled = spin_lock_irqsave(&queue->lock);

	/* A work already in the ring runs once for both requests. */
	if (work->queued != 0) {
		spin_unlock_irqrestore(&queue->lock, enabled);
		return 0;
	}

	/* XXX: a full ring drops the request without any report. */
	if (queue->count >= I915_WORKQUEUE_DEPTH) {
		spin_unlock_irqrestore(&queue->lock, enabled);
		return 0;
	}

	/*
	 * The work is now in the ring.  A running work stays RUNNING and the
	 * worker makes it PENDING again once its callback returns.
	 */
	work->queue = queue;
	work->queued = 1;
	if (work->state == I915_WORK_IDLE)
		work->state = I915_WORK_PENDING;

	/* Places the work at the tail of the ring. */
	queue->slots[queue->tail] = work;
	queue->tail = (queue->tail + 1U) % I915_WORKQUEUE_DEPTH;
	queue->count++;

	/* Wakes the worker for the new work. */
	waitq_wake_all(&queue->waitq);

	spin_unlock_irqrestore(&queue->lock, enabled);

	/* Succeeded: the work will run. */
	return 1;
}

/*
 * Removes a work from its queue if it is still pending.
 *
 * Does not wait for a running callback.  Returns 1 when a pending work was
 * removed, or 0 otherwise.
 */
int
drv_i915_cancel_work(
	struct i915_workqueue *queue,
	struct i915_work *work)
{
	unsigned long enabled;
	int removed;

	/* Takes the work out of the ring. */
	enabled = spin_lock_irqsave(&queue->lock);

	removed = i915_workqueue_remove(queue, work);

	spin_unlock_irqrestore(&queue->lock, enabled);

	/* The work was not pending. */
	if (removed == 0)
		return 0;

	/* Succeeded: the pending work will not run. */
	return 1;
}

/*
 * Removes a pending work and waits until its callback is not running.
 *
 * The wait is bounded by an absolute scheduler-tick deadline, which is a
 * safety limit rather than a normal outcome.  Returns 1 when a pending work
 * was removed, or 0 otherwise.  The caller must not hold a lock the
 * callback takes.
 */
int
drv_i915_cancel_work_sync(
	struct i915_workqueue *queue,
	struct i915_work *work,
	uint64_t deadline)
{
	unsigned long enabled;
	uint64_t observed;
	uint64_t now;
	int removed;

	/* Takes the work out of the ring and waits out a running callback. */
	enabled = spin_lock_irqsave(&queue->lock);

	removed = i915_workqueue_remove(queue, work);

	/* Sleeps until the callback has returned or the safety limit passes. */
	for (;;) {
		/* The callback is not running, which is what the caller needs. */
		if (work->state != I915_WORK_RUNNING)
			break;

		/* XXX: the safety limit returns with the callback still running. */
		now = sched_ticks();
		if (now >= deadline)
			break;

		/* Sleeps until the worker reports the callback finished. */
		observed = waitq_sequence(&work->done_waitq);
		(void)waitq_sleep(&work->done_waitq, &queue->lock, observed, deadline, 0U);
	}

	spin_unlock_irqrestore(&queue->lock, enabled);

	/* The work was not pending. */
	if (removed == 0)
		return 0;

	/* Succeeded: the pending work will not run. */
	return 1;
}

/*
 * Waits until a queued or running work has finished, without cancelling it.
 *
 * The wait is bounded by an absolute scheduler-tick deadline, which is a
 * safety limit rather than a normal outcome.  Returns 1 when the work was
 * pending or running, or 0 when it was already idle, which is not a
 * failure.
 */
int
drv_i915_flush_work(
	struct i915_workqueue *queue,
	struct i915_work *work,
	uint64_t deadline)
{
	unsigned long enabled;
	uint64_t observed;
	uint64_t now;
	int active;

	/* Waits for the work to leave PENDING and RUNNING, leaving it queued. */
	enabled = spin_lock_irqsave(&queue->lock);

	/* Notes whether the flush had anything to wait for. */
	active = 0;
	if (work->state != I915_WORK_IDLE)
		active = 1;

	/* Sleeps until the work is idle or the safety limit passes. */
	for (;;) {
		/* The work has run and was not queued again. */
		if (work->state == I915_WORK_IDLE)
			break;

		/* XXX: the safety limit returns with the work still unfinished. */
		now = sched_ticks();
		if (now >= deadline)
			break;

		/* Sleeps until the worker reports a callback finished. */
		observed = waitq_sequence(&work->done_waitq);
		(void)waitq_sleep(&work->done_waitq, &queue->lock, observed, deadline, 0U);
	}

	spin_unlock_irqrestore(&queue->lock, enabled);

	/* The work was already idle. */
	if (active == 0)
		return 0;

	/* Succeeded: the work that was pending or running has finished. */
	return 1;
}

/*
 * Reports whether a work sits in its queue.
 *
 * A running work that queued itself again counts as pending.
 */
int
drv_i915_work_pending(
	struct i915_workqueue *queue,
	struct i915_work *work)
{
	unsigned long enabled;
	int queued;

	/* Samples the work's place in the ring. */
	enabled = spin_lock_irqsave(&queue->lock);

	queued = work->queued;

	spin_unlock_irqrestore(&queue->lock, enabled);

	/* The work is not in the ring. */
	if (queued == 0)
		return 0;

	/* Succeeded: the work waits for the worker. */
	return 1;
}

/*
 * Creates a timer queue that feeds a work queue and starts its timer thread.
 *
 * Returns 0, or the error of the thread creation; a failed queue has no
 * thread and needs no destroy.
 */
int
drv_i915_timer_queue_create(
	struct i915_timer_queue *timers,
	struct i915_workqueue *workqueue,
	const char *name)
{
	struct thread *thread;
	unsigned slot;
	int error;

	/* Prepares the lock and the queue the timer thread sleeps on. */
	spin_init(&timers->lock, LOCK_RANK_DEVICE, name);
	waitq_init(&timers->waitq, name);

	/* Starts with no work armed. */
	for (slot = 0U; slot < I915_TIMER_QUEUE_SLOTS; slot++)
		timers->armed[slot] = NULL;

	/*
	 * The thread counts as alive from here on, so a destroy that follows
	 * a successful create always waits for it to leave.
	 */
	timers->workqueue = workqueue;
	timers->stop = 0;
	timers->alive = 1;
	timers->thread = NULL;
	timers->name = name;

	/* Creates the thread that fires the armed works. */
	error = kthread_create(i915_timer_thread, timers, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		timers->alive = 0;
		return error;
	}

	/* The thread reclaims itself when it leaves. */
	timers->thread = thread;
	thread->detached = 1U;
	thread_start(thread);

	/* Succeeded: the timer thread runs and waits for armed works. */
	return 0;
}

/*
 * Stops a timer queue's thread and waits until it has left.
 *
 * Works still armed are dropped without running; the owner cancels them
 * first.
 */
void
drv_i915_timer_queue_destroy(
	struct i915_timer_queue *timers)
{
	uint64_t observed;
	unsigned long enabled;

	/* Asks the timer thread to leave and waits until it has. */
	enabled = spin_lock_irqsave(&timers->lock);

	timers->stop = 1;
	waitq_wake_all(&timers->waitq);

	/* Sleeps until the thread reports that it has left its loop. */
	while (timers->alive != 0) {
		observed = waitq_sequence(&timers->waitq);
		(void)waitq_sleep(&timers->waitq, &timers->lock, observed, 0U, 0U);
	}

	spin_unlock_irqrestore(&timers->lock, enabled);
}

/*
 * Prepares an idle delayed work that runs a callback with an argument.
 */
void
drv_i915_delayed_work_init(
	struct i915_delayed_work *delayed,
	void (*function)(void *),
	void *context)
{
	/* Prepares the work handed to the work queue once the delay elapses. */
	drv_i915_work_init(&delayed->work, function, context);

	/* Starts disarmed, on no timer queue. */
	delayed->timers = NULL;
	delayed->armed = 0;
	delayed->firing = 0;
	delayed->deadline = 0U;

	/* Starts with clean counters. */
	delayed->armed_count = 0U;
	delayed->fired_count = 0U;
	delayed->cancelled_armed = 0U;
	delayed->cancelled_pending = 0U;
}

/*
 * Arms a delayed work to run after a number of milliseconds.
 *
 * Returns 1 when the work was newly armed, or 0 when it was already armed
 * or pending (and is left alone), when the deadline cannot be computed, or
 * when no timer slot is free.
 */
int
drv_i915_delayed_queue(
	struct i915_timer_queue *timers,
	struct i915_delayed_work *delayed,
	unsigned delay_ms)
{
	uint64_t now;
	uint64_t ticks;
	uint64_t deadline;
	unsigned slot;
	int pending;
	int error;
	unsigned long enabled;

	/* Arms the work in a free slot and wakes the timer thread. */
	enabled = spin_lock_irqsave(&timers->lock);

	/* Lets a hand-over in progress finish, so the work is either armed or pending. */
	i915_delayed_wait_not_firing(timers, delayed);

	/* An armed work keeps its earlier deadline. */
	if (delayed->armed != 0) {
		spin_unlock_irqrestore(&timers->lock, enabled);
		return 0;
	}

	/* A work already in the work queue runs once for both requests. */
	pending = drv_i915_work_pending(timers->workqueue, &delayed->work);
	if (pending != 0) {
		spin_unlock_irqrestore(&timers->lock, enabled);
		return 0;
	}

	/*
	 * Converts the delay into scheduler ticks, never earlier than asked:
	 * the current tick is partly used, hence one tick more.
	 */
	now = sched_ticks();
	ticks = kern_ms_to_ticks(delay_ms) + 1U;
	deadline = 0U;
	error = kern_deadline_after(now, ticks, &deadline);
	if (error != 0) {
		spin_unlock_irqrestore(&timers->lock, enabled);
		return 0;
	}

	/* Finds a free slot for the work. */
	for (slot = 0U; slot < I915_TIMER_QUEUE_SLOTS; slot++) {
		if (timers->armed[slot] == NULL)
			break;
	}

	/* Every slot holds an armed work. */
	if (slot == I915_TIMER_QUEUE_SLOTS) {
		spin_unlock_irqrestore(&timers->lock, enabled);
		kern_logf("i915: delayed work: no free timer slot (%s)\n", timers->name);
		return 0;
	}

	/* The work now waits for its deadline in the slot. */
	delayed->timers = timers;
	delayed->deadline = deadline;
	delayed->armed = 1;
	delayed->armed_count++;
	timers->armed[slot] = delayed;

	/* Wakes the timer thread, whose earliest deadline may have moved. */
	waitq_wake_all(&timers->waitq);

	spin_unlock_irqrestore(&timers->lock, enabled);

	/* Succeeded: the work runs once its deadline passes. */
	return 1;
}

/*
 * Disarms or dequeues a delayed work.
 *
 * Does not wait for a running callback.  Returns 1 when an armed or pending
 * work was stopped, or 0 otherwise.
 */
int
drv_i915_delayed_cancel(
	struct i915_timer_queue *timers,
	struct i915_delayed_work *delayed)
{
	int disarmed;
	int removed;
	unsigned long enabled;

	/* Takes the work off its timer slot. */
	enabled = spin_lock_irqsave(&timers->lock);

	i915_delayed_wait_not_firing(timers, delayed);
	disarmed = i915_delayed_disarm(timers, delayed);
	if (disarmed != 0)
		delayed->cancelled_armed++;

	spin_unlock_irqrestore(&timers->lock, enabled);

	/* An armed work never reached the work queue. */
	if (disarmed != 0)
		return 1;

	/* Takes a work already handed over out of the work queue. */
	removed = drv_i915_cancel_work(timers->workqueue, &delayed->work);
	if (removed == 0)
		return 0;

	/* Counts a cancel that caught the work in the work queue. */
	delayed->cancelled_pending++;

	/* Succeeded: the pending work will not run. */
	return 1;
}

/*
 * Disarms or dequeues a delayed work and waits until it is not running.
 *
 * The wait is bounded by an absolute scheduler-tick deadline, which is a
 * safety limit rather than a normal outcome.  Returns 1 when an armed or
 * pending work was stopped, or 0 otherwise.  The caller must not hold a
 * lock the callback takes.
 */
int
drv_i915_delayed_cancel_sync(
	struct i915_timer_queue *timers,
	struct i915_delayed_work *delayed,
	uint64_t deadline)
{
	int disarmed;
	int removed;
	unsigned long enabled;

	/* Takes the work off its timer slot. */
	enabled = spin_lock_irqsave(&timers->lock);

	i915_delayed_wait_not_firing(timers, delayed);
	disarmed = i915_delayed_disarm(timers, delayed);
	if (disarmed != 0)
		delayed->cancelled_armed++;

	spin_unlock_irqrestore(&timers->lock, enabled);

	/*
	 * Takes the work out of the work queue and waits out a running
	 * callback, even when the work was armed: an earlier run may still be
	 * executing.
	 */
	removed = drv_i915_cancel_work_sync(timers->workqueue, &delayed->work, deadline);
	if (removed != 0)
		delayed->cancelled_pending++;

	/* The work was neither armed nor pending. */
	if (disarmed == 0 && removed == 0)
		return 0;

	/* Succeeded: the armed or pending work will not run. */
	return 1;
}

/*
 * Reports whether a delayed work is armed or pending.
 *
 * A work whose callback is running is not pending unless it was queued
 * again.
 */
int
drv_i915_delayed_pending(
	struct i915_timer_queue *timers,
	struct i915_delayed_work *delayed)
{
	int waiting;
	int queued;
	unsigned long enabled;

	/* Samples whether the work waits for its deadline or is being handed over. */
	enabled = spin_lock_irqsave(&timers->lock);

	waiting = 0;
	if (delayed->armed != 0)
		waiting = 1;
	else if (delayed->firing != 0)
		waiting = 1;

	spin_unlock_irqrestore(&timers->lock, enabled);

	/* A work still waiting for its deadline is pending. */
	if (waiting != 0)
		return 1;

	/* Samples whether the work sits in the work queue. */
	queued = drv_i915_work_pending(timers->workqueue, &delayed->work);
	if (queued == 0)
		return 0;

	/* Succeeded: the work waits for the worker. */
	return 1;
}

/* Runs the queued works in order until a destroy asks the worker to leave. */
static void
i915_workqueue_worker(
	void *argument)
{
	struct i915_workqueue *queue;
	struct i915_work *work;
	unsigned long enabled;
	uint64_t observed;

	/* The worker was created for exactly this queue. */
	queue = argument;

	/* Drains the ring, dropping the lock around each callback. */
	enabled = spin_lock_irqsave(&queue->lock);

	for (;;) {
		/* Leaves once a stop was asked and nothing is left to run. */
		if (queue->stop != 0 && queue->count == 0U)
			break;

		/* Sleeps until work is queued or a stop is asked. */
		if (queue->count == 0U) {
			observed = waitq_sequence(&queue->waitq);
			(void)waitq_sleep(&queue->waitq, &queue->lock, observed, 0U, 0U);
			continue;
		}

		/* Takes the oldest work off the ring. */
		work = queue->slots[queue->head];
		queue->slots[queue->head] = NULL;
		queue->head = (queue->head + 1U) % I915_WORKQUEUE_DEPTH;
		queue->count--;

		/*
		 * The work leaves the pending state before its callback runs, so
		 * a callback that queues its own work again runs once more.
		 */
		work->queued = 0;
		work->state = I915_WORK_RUNNING;
		work->ran_count++;

		spin_unlock_irqrestore(&queue->lock, enabled);

		/* Runs the callback with no lock held. */
		if (work->function != NULL)
			work->function(work->context);

		enabled = spin_lock_irqsave(&queue->lock);

		/* A work its callback queued again is pending; otherwise it is idle. */
		if (work->queued != 0) {
			work->state = I915_WORK_PENDING;
		} else {
			work->state = I915_WORK_IDLE;
		}

		/* Wakes a synchronous cancel or a flush waiting for the callback. */
		waitq_wake_all(&work->done_waitq);
	}

	/* Tells the destroy that the worker has left its loop. */
	queue->worker_alive = 0;
	waitq_wake_all(&queue->waitq);

	spin_unlock_irqrestore(&queue->lock, enabled);
}

/* Removes a pending work from the ring; the caller holds the queue lock. */
static int
i915_workqueue_remove(
	struct i915_workqueue *queue,
	struct i915_work *work)
{
	unsigned position;
	unsigned index;
	unsigned to;
	unsigned from;

	/* A work outside the ring has nothing to remove. */
	if (work->queued == 0)
		return 0;

	/* Finds the work's position counted from the head. */
	for (position = 0U; position < queue->count; position++) {
		index = (queue->head + position) % I915_WORKQUEUE_DEPTH;
		if (queue->slots[index] == work)
			break;
	}

	/* The work claims to be queued but is not in the ring. */
	if (position == queue->count)
		return 0;

	/* Closes the gap by moving every later work one slot toward the head. */
	for (; position + 1U < queue->count; position++) {
		to = (queue->head + position) % I915_WORKQUEUE_DEPTH;
		from = (queue->head + position + 1U) % I915_WORKQUEUE_DEPTH;
		queue->slots[to] = queue->slots[from];
	}
	queue->tail = (queue->tail + I915_WORKQUEUE_DEPTH - 1U) % I915_WORKQUEUE_DEPTH;
	queue->count--;

	/* The work is out of the ring; a running callback keeps running. */
	work->queued = 0;
	if (work->state == I915_WORK_PENDING)
		work->state = I915_WORK_IDLE;

	/* Succeeded: the work will not run for this request. */
	return 1;
}

/* Hands due works to the work queue until a destroy asks the thread to leave. */
static void
i915_timer_thread(
	void *argument)
{
	struct i915_timer_queue *timers;
	struct i915_delayed_work *due;
	uint64_t earliest;
	uint64_t now;
	uint64_t observed;
	unsigned long enabled;

	/* The thread was created for exactly this timer queue. */
	timers = argument;

	/* Fires due works, dropping the lock around each hand-over. */
	enabled = spin_lock_irqsave(&timers->lock);

	for (;;) {
		/* Leaves at the destroy's request. */
		if (timers->stop != 0)
			break;

		/* Takes a due work, or learns the earliest deadline still ahead. */
		now = sched_ticks();
		due = i915_timer_take_due(timers, now, &earliest);

		/* Sleeps until the earliest deadline, or until woken when nothing is armed. */
		if (due == NULL) {
			observed = waitq_sequence(&timers->waitq);
			(void)waitq_sleep(&timers->waitq, &timers->lock, observed, earliest, 0U);
			continue;
		}

		/*
		 * The timer queue lock is not held across the work queue lock:
		 * firing keeps queue and cancel away until the work is PENDING.
		 */
		due->armed = 0;
		due->firing = 1;
		due->fired_count++;

		spin_unlock_irqrestore(&timers->lock, enabled);

		/* Hands the due work to the worker. */
		(void)drv_i915_queue_work(timers->workqueue, &due->work);

		enabled = spin_lock_irqsave(&timers->lock);

		/* The hand-over is over; wakes a queue or cancel waiting for it. */
		due->firing = 0;
		waitq_wake_all(&timers->waitq);
	}

	/* Tells the destroy that the thread has left its loop. */
	timers->alive = 0;
	waitq_wake_all(&timers->waitq);

	spin_unlock_irqrestore(&timers->lock, enabled);
}

/* Takes the first due work off its slot; the caller holds the timer queue lock. */
static struct i915_delayed_work *
i915_timer_take_due(
	struct i915_timer_queue *timers,
	uint64_t now,
	uint64_t *earliest)
{
	struct i915_delayed_work *delayed;
	unsigned slot;

	/*
	 * Scans the slots for a due work, noting the earliest deadline of the
	 * works before it; zero means no deadline, which sleeps until woken.
	 */
	*earliest = 0U;
	delayed = NULL;
	for (slot = 0U; slot < I915_TIMER_QUEUE_SLOTS; slot++) {
		delayed = timers->armed[slot];
		if (delayed == NULL)
			continue;

		/* Stops at the first work whose deadline has come. */
		if (delayed->deadline <= now)
			break;

		/* Keeps the earliest deadline still ahead. */
		if (*earliest == 0U || delayed->deadline < *earliest)
			*earliest = delayed->deadline;
	}

	/* No armed work is due yet. */
	if (slot == I915_TIMER_QUEUE_SLOTS)
		return NULL;

	/* The due work leaves its slot to be handed over. */
	timers->armed[slot] = NULL;

	/* Succeeded: the work is due. */
	return delayed;
}

/* Waits out a hand-over of the work in progress; the caller holds the timer queue lock. */
static void
i915_delayed_wait_not_firing(
	struct i915_timer_queue *timers,
	struct i915_delayed_work *delayed)
{
	uint64_t observed;

	/* Sleeps until the timer thread reports the hand-over finished. */
	while (delayed->firing != 0) {
		observed = waitq_sequence(&timers->waitq);
		(void)waitq_sleep(&timers->waitq, &timers->lock, observed, 0U, 0U);
	}
}

/* Takes an armed work off its slot; the caller holds the timer queue lock. */
static int
i915_delayed_disarm(
	struct i915_timer_queue *timers,
	struct i915_delayed_work *delayed)
{
	unsigned slot;

	/* A work that is not armed occupies no slot. */
	if (delayed->armed == 0)
		return 0;

	/* Clears every slot that names the work. */
	for (slot = 0U; slot < I915_TIMER_QUEUE_SLOTS; slot++) {
		if (timers->armed[slot] == delayed)
			timers->armed[slot] = NULL;
	}

	/* The work no longer waits for its deadline. */
	delayed->armed = 0;

	/* Succeeded: the armed work will not fire. */
	return 1;
}
