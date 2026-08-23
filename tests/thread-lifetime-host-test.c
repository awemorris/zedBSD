/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <kern/process.h>
#include <kern/test-checkpoint.h>
#include <kern/thread.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

static mtx_t kernel_mutex;
static struct thread observer;
static struct thread *watched_thread;
static struct process *watched_process;
static unsigned reap_at_publish;
static unsigned thread_freed;
static unsigned process_freed;
static unsigned process_retired_calls;

hal_task_t hal_task_get_current(void) { return (hal_task_t)&observer; }
void *hal_task_get_private(hal_task_t task) { return task; }

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	(void)lock;
	assert(mtx_lock(&kernel_mutex) == thrd_success);
	return 0;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long irq)
{
	(void)lock;
	(void)irq;
	assert(mtx_unlock(&kernel_mutex) == thrd_success);
}

void
waitq_wake_all(struct wait_queue *queue)
{
	queue->sequence++;
}

void sched_wakeup(struct thread *thread) { (void)thread; }
void hal_task_set_private(hal_task_t task, void *value)
{ (void)task; (void)value; }
void hal_task_destroy(hal_task_t task) { (void)task; }

void
process_ref(struct process *process)
{
	refcount_get(&process->refs);
}

void
process_release(struct process *process)
{
	if (!refcount_put(&process->refs))
		return;
	assert(process == watched_process && process->state == PROCESS_DEAD);
	process_freed++;
}

void
process_thread_retired(struct thread *thread)
{
	assert(thread == watched_thread);
	assert(!thread_freed && !process_freed);
	assert(refcount_load(&thread->refs) >= 1);
	assert(refcount_load(&watched_process->refs) == 2);
	process_retired_calls++;
	/* Model the last-thread process publication followed by a remote process
	 * reap before this retired hook has returned.  The hook-owned process ref
	 * must keep the allocation alive through detached self-reaping as well. */
	watched_process->state = PROCESS_DEAD;
	process_release(watched_process);
	assert(!process_freed);
}

void
kern_free(void *pointer)
{
	if (pointer == watched_thread)
		thread_freed++;
}

void
hal_fatal(const char *file, int line, const char *message)
{
	fprintf(stderr, "unexpected HAL fatal at %s:%d: %s\n", file, line,
	    message);
	abort();
}

static void
checkpoint(enum kern_test_checkpoint_id id, void *object, void *argument)
{
	(void)argument;
	if (id != KERN_TEST_THREAD_RETIRED_AFTER_PUBLISH ||
	    object != watched_thread)
		return;
	assert(watched_thread->state == THREAD_ZOMBIE);
	assert(refcount_load(&watched_thread->refs) == 2);
	assert(refcount_load(&watched_process->refs) == 2);
	if (reap_at_publish) {
		assert(thread_wait(watched_thread, NULL) == 0);
		assert(watched_thread->state == THREAD_DEAD);
		assert(refcount_load(&watched_thread->refs) == 1);
		assert(!thread_freed);
	}
}

static void
run_retirement(unsigned detached, unsigned remote_reap)
{
	struct process process;
	struct thread thread;

	memset(&process, 0, sizeof(process));
	memset(&thread, 0, sizeof(thread));
	refcount_init(&process.refs, 1);
	refcount_init(&thread.refs, 1);
	process.state = PROCESS_EXITING;
	process.threads = &thread;
	process.thread_count = 1;
	thread.proc = &process;
	thread.task = (hal_task_t)&thread;
	thread.state = THREAD_EXITING;
	thread.detached = detached;
	thread.join_waitq.sequence = 1;
	thread.join_waitq.name = "retired join";
	watched_thread = &thread;
	watched_process = &process;
	reap_at_publish = remote_reap;
	thread_freed = process_freed = process_retired_calls = 0;
	thread_sched_retired(&thread);
	assert(process_retired_calls == 1);
	assert(thread.state == THREAD_DEAD);
	assert(thread_freed == 1);
	assert(process_freed == 1);
}

int
main(void)
{
	assert(mtx_init(&kernel_mutex, mtx_plain) == thrd_success);
	kern_test_checkpoint_set(checkpoint, NULL);
	run_retirement(0, 1);
	run_retirement(1, 0);
	kern_test_checkpoint_set(NULL, NULL);
	puts("zedBSD retired thread ownership host tests: PASS");
	return 0;
}
