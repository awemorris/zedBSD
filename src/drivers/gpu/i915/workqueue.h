/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Deferred work and delayed work.
 *
 * A work queue owns one worker thread that runs queued callbacks in order
 * with no lock held: a work is
 * PENDING while it sits in the queue and RUNNING while its callback
 * executes.  The pending state is cleared before the callback runs, so a
 * callback may queue its own work again and it runs once more.
 *
 * A timer queue adds delayed work on top of one work queue.
 * Its timer thread sleeps on an ordinary wait queue until the earliest
 * deadline, so the scheduler tick wakes it and nothing runs in the timer
 * interrupt, then hands the due work to the work queue:
 *
 *   IDLE -> ARMED (waiting for its deadline) -> PENDING (in the work queue)
 *        -> RUNNING -> IDLE        (a running work may be armed again)
 *
 *   queue        1 = newly armed; 0 = already armed or pending (left alone)
 *   cancel       disarms or dequeues; does not wait for a running callback
 *   cancel_sync  the same, then waits until the callback is not running;
 *                the caller must not hold a lock the callback takes
 */

#ifndef DRIVERS_GPU_I915_WORKQUEUE_H
#define DRIVERS_GPU_I915_WORKQUEUE_H

#include <kern/lock.h>
#include <kern/waitq.h>
#include <stdint.h>

struct thread;
struct i915_workqueue;
struct i915_timer_queue;

/* How many works one work queue holds at once. */
#define I915_WORKQUEUE_DEPTH	64U

/* How many delayed works one timer queue keeps armed at once. */
#define I915_TIMER_QUEUE_SLOTS	16U

/* Where a work stands between being queued and having run. */
enum i915_work_state {
	/* Neither queued nor running. */
	I915_WORK_IDLE = 0,

	/* In the queue, waiting for the worker. */
	I915_WORK_PENDING,

	/* Its callback executes; it may be queued again meanwhile. */
	I915_WORK_RUNNING
};

/*
 * One piece of deferred work.
 *
 * It lives inside its owner, which prepares it once with
 * drv_i915_work_init() and must cancel it synchronously before freeing it.
 * The state fields are protected by the lock of the queue it was last
 * queued on.
 */
struct i915_work {
	/* The callback the worker runs, and the argument it receives. */
	void (*function)(void *context);
	void *context;

	/* One of enum i915_work_state. */
	int state;

	/* Nonzero while the work sits in the queue's ring. */
	int queued;

	/* How many times the worker has started the callback. */
	int ran_count;

	/* The queue the work was last queued on. */
	struct i915_workqueue *queue;

	/* Where a synchronous cancel or a flush waits for the callback to finish. */
	struct wait_queue done_waitq;
};

/*
 * One queue of deferred work and the worker thread that drains it.
 *
 * It lives inside its owner from drv_i915_workqueue_create() to
 * drv_i915_workqueue_destroy().  The lock is taken with interrupts
 * disabled, so work may be queued from an interrupt handler.
 */
struct i915_workqueue {
	/* Protects the ring, the stop request and every queued work's state. */
	struct spinlock lock;

	/* Where the worker sleeps for new work, and the destroyer for the worker's exit. */
	struct wait_queue waitq;

	/* The queued works in order: count entries starting at head. */
	struct i915_work *slots[I915_WORKQUEUE_DEPTH];
	unsigned head;
	unsigned tail;
	unsigned count;

	/* Nonzero once a destroy asked the worker to leave. */
	int stop;

	/* Nonzero until the worker has left its loop. */
	int worker_alive;

	/* The worker thread, which reclaims itself when it leaves. */
	struct thread *worker;

	/* The queue's name in kernel diagnostics. */
	const char *name;
};

/*
 * One work that runs once its delay has elapsed.
 *
 * It lives inside its owner, which prepares it once with
 * drv_i915_delayed_work_init() and must cancel it synchronously before
 * freeing it.  The arming fields are protected by the timer queue's lock.
 */
struct i915_delayed_work {
	/* The work handed to the work queue once the deadline passes. */
	struct i915_work work;

	/* The timer queue the work was last armed on. */
	struct i915_timer_queue *timers;

	/* Nonzero while the work waits for its deadline. */
	int armed;

	/*
	 * Nonzero while the timer thread hands the work to the work queue;
	 * queue and cancel wait until the work is PENDING.
	 */
	int firing;

	/* The scheduler tick at which the work becomes due. */
	uint64_t deadline;

	/* How often the work was armed, handed over, and cancelled in each state. */
	unsigned armed_count;
	unsigned fired_count;
	unsigned cancelled_armed;
	unsigned cancelled_pending;
};

/*
 * The armed delayed works of one owner and the timer thread that fires them.
 *
 * It lives inside its owner from drv_i915_timer_queue_create() to
 * drv_i915_timer_queue_destroy() and feeds one work queue, which must
 * outlive it.
 */
struct i915_timer_queue {
	/* Protects the armed slots, the stop request and every armed work. */
	struct spinlock lock;

	/* Where the timer thread sleeps until the earliest deadline or a change. */
	struct wait_queue waitq;

	/* The armed works; an empty slot is NULL. */
	struct i915_delayed_work *armed[I915_TIMER_QUEUE_SLOTS];

	/* The work queue due works are handed to. */
	struct i915_workqueue *workqueue;

	/* Nonzero once a destroy asked the timer thread to leave. */
	int stop;

	/* Nonzero until the timer thread has left its loop. */
	int alive;

	/* The timer thread, which reclaims itself when it leaves. */
	struct thread *thread;

	/* The queue's name in kernel diagnostics and in the log. */
	const char *name;
};

void drv_i915_work_init(struct i915_work *work, void (*function)(void *), void *context);
int drv_i915_workqueue_create(struct i915_workqueue *queue, const char *name);
void drv_i915_workqueue_destroy(struct i915_workqueue *queue);
int drv_i915_queue_work(struct i915_workqueue *queue, struct i915_work *work);
int drv_i915_cancel_work(struct i915_workqueue *queue, struct i915_work *work);
int drv_i915_cancel_work_sync(struct i915_workqueue *queue, struct i915_work *work, uint64_t deadline);
int drv_i915_flush_work(struct i915_workqueue *queue, struct i915_work *work, uint64_t deadline);
int drv_i915_work_pending(struct i915_workqueue *queue, struct i915_work *work);

int drv_i915_timer_queue_create(struct i915_timer_queue *timers, struct i915_workqueue *workqueue, const char *name);
void drv_i915_timer_queue_destroy(struct i915_timer_queue *timers);
void drv_i915_delayed_work_init(struct i915_delayed_work *delayed, void (*function)(void *), void *context);
int drv_i915_delayed_queue(struct i915_timer_queue *timers, struct i915_delayed_work *delayed, unsigned delay_ms);
int drv_i915_delayed_cancel(struct i915_timer_queue *timers, struct i915_delayed_work *delayed);
int drv_i915_delayed_cancel_sync(struct i915_timer_queue *timers, struct i915_delayed_work *delayed, uint64_t deadline);
int drv_i915_delayed_pending(struct i915_timer_queue *timers, struct i915_delayed_work *delayed);

#endif
