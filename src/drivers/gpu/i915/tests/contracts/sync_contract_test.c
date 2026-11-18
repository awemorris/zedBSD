/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The completion and deferred-work contract, checked on the host.
 *
 * Runs the production completion of sync.c and the production work queue of
 * workqueue.c against the single-threaded stand-in kernel of
 * host_kernel.c.  The completion checks cover a signal before the wait, a
 * wait that times out, a signal that arrives while the waiter sleeps (the
 * sleep hook stands for the interrupt), the permit count, and the reinit.
 * The work queue checks cover what holds without the worker thread ever
 * running: a queued work is deferred, not run inline; a second queue of a
 * pending work is refused; a cancel takes a pending work out; and a
 * synchronous cancel of a pending work returns without waiting.  The checks
 * that need the worker to run are described in README.md.
 */

#include "contract.h"
#include "host_kernel.h"

#include "../../sync.h"
#include "../../workqueue.h"

#include <stddef.h>
#include <stdint.h>

/*
 * A signal that arrives while a waiter sleeps, as an interrupt would.
 *
 * It lives on the stack of one check and is handed to the sleep hook.
 */
struct sync_test_irq {
	/* The completion the interrupt signals. */
	struct i915_completion *completion;

	/* The sleep on which the interrupt arrives. */
	int fire_after;

	/* How many sleeps have passed so far. */
	int sleeps;
};

/*
 * How many times the plain work callback has run.
 *
 * The worker never runs on the host, so it stays zero; a nonzero value
 * would mean a queue ran its work inline.
 */
static int sync_test_ran_count;

static void sync_test_irq_hook(void *context);
static void sync_test_work(void *context);
static void sync_check_completion_ready(void);
static void sync_check_completion_timeout(void);
static void sync_check_completion_during_wait(void);
static void sync_check_completion_count(void);
static void sync_check_completion_reinit(void);
static void sync_check_queue_defers(struct i915_workqueue *queue);
static void sync_check_cancel(struct i915_workqueue *queue);
static void sync_check_double_queue(struct i915_workqueue *queue);
static void sync_check_cancel_sync(struct i915_workqueue *queue);

/*
 * Runs the completion and deferred-work contract checks.
 */
int
main(void)
{
	static struct i915_workqueue queue;
	unsigned created;
	unsigned started;
	int error;
	int status;

	contract_begin("sync / deferred-work contract tests (GPU-free)");

	/* Starts the stand-in kernel at tick zero with nothing held. */
	host_kernel_reset();

	/* Runs the completion groups. */
	sync_check_completion_ready();
	sync_check_completion_timeout();
	sync_check_completion_during_wait();
	sync_check_completion_count();
	sync_check_completion_reinit();

	/* Creates the work queue whose worker never runs on the host. */
	error = drv_i915_workqueue_create(&queue, "contract-wq");
	contract_check(error == 0, "work queue created");
	created = host_thread_created();
	started = host_thread_started();
	contract_check(created == 1U, "one worker thread created");
	contract_check(started == 1U, "the worker thread was started");

	/* Runs the work queue groups on that queue. */
	sync_check_queue_defers(&queue);
	sync_check_cancel(&queue);
	sync_check_double_queue(&queue);
	sync_check_cancel_sync(&queue);

	/*
	 * The queue is not destroyed: the destroy waits for the worker to
	 * leave, and on the host the worker never ran.  It owns no memory.
	 */

	/* Reports whether every check held. */
	status = contract_end();
	if (status != 0)
		return status;

	/* Succeeded: the completion and the work queue keep their contract. */
	return 0;
}

/* Signals the completion on the sleep the check chose. */
static void
sync_test_irq_hook(
	void *context)
{
	struct sync_test_irq *irq;

	/* Counts the sleep and fires on the chosen one. */
	irq = context;
	irq->sleeps++;
	if (irq->sleeps == irq->fire_after)
		drv_i915_complete(irq->completion);
}

/* Counts one run of the plain work. */
static void
sync_test_work(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* A run on the host would mean the queue ran its work inline. */
	sync_test_ran_count++;
}

/* Checks that a signal before the wait is consumed without sleeping. */
static void
sync_check_completion_ready(void)
{
	struct i915_completion completion;
	unsigned sleeps_before;
	unsigned sleeps_after;
	uint64_t deadline;
	int completed;
	int depth;

	contract_section("SYNC-1: completion signalled before wait returns immediately");

	/* Signals the completion before anyone waits. */
	drv_i915_completion_init(&completion, "contract-sync1");
	drv_i915_complete(&completion);

	/* The wait consumes the signal at once. */
	sleeps_before = host_kernel_sleeps();
	deadline = host_kernel_ticks() + 10U;
	completed = drv_i915_wait_for_completion(&completion, deadline);
	contract_check(completed == 1, "already-done wait reports the signal");
	sleeps_after = host_kernel_sleeps();
	contract_check(sleeps_after == sleeps_before, "no sleep taken");

	/* The completion gives its lock back. */
	depth = host_kernel_lock_depth();
	contract_check(depth == 0, "no lock left held");
}

/* Checks that a wait without a signal ends at its deadline. */
static void
sync_check_completion_timeout(void)
{
	struct i915_completion completion;
	uint64_t deadline;
	uint64_t now;
	int completed;
	int depth;

	contract_section("SYNC-2: wait times out when never completed");

	/* Waits five ticks for a signal that never comes. */
	drv_i915_completion_init(&completion, "contract-sync2");
	deadline = host_kernel_ticks() + 5U;
	completed = drv_i915_wait_for_completion(&completion, deadline);
	contract_check(completed == 0, "timeout returns 0");

	/* The wait lasted until the deadline and left no permit behind. */
	now = host_kernel_ticks();
	contract_check(now >= deadline, "the deadline passed before the wait gave up");
	contract_check(completion.done == 0U, "still not done");
	depth = host_kernel_lock_depth();
	contract_check(depth == 0, "no lock left held");
}

/* Checks that a signal arriving while the waiter sleeps ends the wait. */
static void
sync_check_completion_during_wait(void)
{
	struct i915_completion completion;
	struct sync_test_irq irq;
	uint64_t deadline;
	int completed;
	int depth;

	contract_section("SYNC-3: completion signalled mid-wait is observed");

	/* The interrupt arrives during the third sleep. */
	drv_i915_completion_init(&completion, "contract-sync3");
	irq.completion = &completion;
	irq.fire_after = 3;
	irq.sleeps = 0;
	host_kernel_set_sleep_hook(sync_test_irq_hook, &irq);

	/* The waiter wakes on that signal, well before its deadline. */
	deadline = host_kernel_ticks() + 10U;
	completed = drv_i915_wait_for_completion(&completion, deadline);
	host_kernel_set_sleep_hook(NULL, NULL);
	contract_check(completed == 1, "wait succeeds when completed mid-wait");
	contract_check(irq.sleeps == 3, "completed on the 3rd sleep");
	contract_check(completion.done == 0U, "the signal was consumed");
	depth = host_kernel_lock_depth();
	contract_check(depth == 0, "no lock left held");
}

/* Checks that each signal satisfies exactly one wait. */
static void
sync_check_completion_count(void)
{
	struct i915_completion completion;
	uint64_t deadline;
	int completed;

	contract_section("SYNC-4: N completes satisfy N waits, then timeout");

	/* Two signals are two permits. */
	drv_i915_completion_init(&completion, "contract-sync4");
	drv_i915_complete(&completion);
	drv_i915_complete(&completion);

	/* Two waits consume them and the third finds none. */
	deadline = host_kernel_ticks() + 3U;
	completed = drv_i915_wait_for_completion(&completion, deadline);
	contract_check(completed == 1, "first wait consumes a permit");
	completed = drv_i915_wait_for_completion(&completion, deadline);
	contract_check(completed == 1, "second wait consumes a permit");
	completed = drv_i915_wait_for_completion(&completion, deadline);
	contract_check(completed == 0, "third wait times out (permits exhausted)");
}

/* Checks that a reinit forgets every unconsumed signal. */
static void
sync_check_completion_reinit(void)
{
	struct i915_completion completion;
	uint64_t deadline;
	int completed;

	contract_section("SYNC-5: reinit discards unconsumed signals");

	/* Signals twice, then reinitializes. */
	drv_i915_completion_init(&completion, "contract-sync5");
	drv_i915_complete(&completion);
	drv_i915_complete(&completion);
	drv_i915_reinit_completion(&completion);
	contract_check(completion.done == 0U, "reinit clears the state");

	/* A wait after the reinit sees no signal. */
	deadline = host_kernel_ticks() + 2U;
	completed = drv_i915_wait_for_completion(&completion, deadline);
	contract_check(completed == 0, "after reinit, wait times out");
}

/* Checks that queueing defers the work instead of running it. */
static void
sync_check_queue_defers(
	struct i915_workqueue *queue)
{
	struct i915_work work;
	int queued;
	int pending;
	int removed;

	contract_section("WQ-1: queue_work does not run the work inline");

	/* Queues a work. */
	sync_test_ran_count = 0;
	drv_i915_work_init(&work, sync_test_work, NULL);
	queued = drv_i915_queue_work(queue, &work);
	contract_check(queued == 1, "queued");

	/* The work waits for the worker. */
	contract_check(work.ran_count == 0, "not run inline (work count)");
	contract_check(sync_test_ran_count == 0, "not run inline (callback count)");
	contract_check(work.state == I915_WORK_PENDING, "work is PENDING");
	pending = drv_i915_work_pending(queue, &work);
	contract_check(pending == 1, "one pending");
	contract_check(queue->count == 1U, "one entry in the ring");

	/* Takes the work out again, so the next group starts from an empty ring. */
	removed = drv_i915_cancel_work(queue, &work);
	contract_check(removed == 1, "cleanup cancel removed the work");
}

/* Checks that a cancel takes a pending work out before it runs. */
static void
sync_check_cancel(
	struct i915_workqueue *queue)
{
	struct i915_work work;
	int removed;
	int pending;

	contract_section("WQ-3: cancel before run prevents execution");

	/* Queues a work and cancels it at once. */
	sync_test_ran_count = 0;
	drv_i915_work_init(&work, sync_test_work, NULL);
	(void)drv_i915_queue_work(queue, &work);
	removed = drv_i915_cancel_work(queue, &work);
	contract_check(removed == 1, "cancel removed pending work");

	/* The work is idle and out of the ring, and never ran. */
	contract_check(work.state == I915_WORK_IDLE, "work is IDLE again");
	pending = drv_i915_work_pending(queue, &work);
	contract_check(pending == 0, "no longer pending");
	contract_check(queue->count == 0U, "ring empty");
	contract_check(work.ran_count == 0, "canceled work never ran");

	/* A second cancel finds nothing to remove. */
	removed = drv_i915_cancel_work(queue, &work);
	contract_check(removed == 0, "cancel of an idle work reports 0");
}

/* Checks that a pending work is queued only once. */
static void
sync_check_double_queue(
	struct i915_workqueue *queue)
{
	struct i915_work work;
	int queued;
	int removed;

	contract_section("WQ-5: queueing an already-queued work is a no-op");

	/* The second request finds the work already in the ring. */
	drv_i915_work_init(&work, sync_test_work, NULL);
	queued = drv_i915_queue_work(queue, &work);
	contract_check(queued == 1, "first queue ok");
	queued = drv_i915_queue_work(queue, &work);
	contract_check(queued == 0, "second queue rejected");
	contract_check(queue->count == 1U, "only one entry");

	/* Takes the work out again, so the next group starts from an empty ring. */
	removed = drv_i915_cancel_work(queue, &work);
	contract_check(removed == 1, "cleanup cancel removed the work");
}

/* Checks that a synchronous cancel of a pending work does not wait. */
static void
sync_check_cancel_sync(
	struct i915_workqueue *queue)
{
	struct i915_work work;
	unsigned sleeps_before;
	unsigned sleeps_after;
	uint64_t deadline;
	int removed;
	int pending;
	int depth;

	contract_section("WQ-8: cancel_work_sync removes pending work, guarantees not running");

	/* Queues a work and cancels it synchronously. */
	drv_i915_work_init(&work, sync_test_work, NULL);
	(void)drv_i915_queue_work(queue, &work);
	sleeps_before = host_kernel_sleeps();
	deadline = host_kernel_ticks() + 10U;
	removed = drv_i915_cancel_work_sync(queue, &work, deadline);
	contract_check(removed == 1, "sync-cancel reports it was active");

	/* A pending callback is not running, so there was nothing to wait out. */
	contract_check(work.state != I915_WORK_RUNNING, "not running on return");
	sleeps_after = host_kernel_sleeps();
	contract_check(sleeps_after == sleeps_before, "no wait for a callback that never started");
	pending = drv_i915_work_pending(queue, &work);
	contract_check(pending == 0, "no longer pending");
	contract_check(work.ran_count == 0, "never ran after sync-cancel");

	/* The queue gives its lock back after every call. */
	depth = host_kernel_lock_depth();
	contract_check(depth == 0, "no lock left held");
}
