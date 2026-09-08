/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Kernel and user threads.
 *
 * A thread wraps a HAL task, carries a process-unique identifier from a
 * small registry, and belongs to a process's thread list.  Retirement
 * publishes the zombie state so that a joiner or the reaper can claim the
 * thread exactly once.  Each secondary CPU gets a preallocated idle
 * thread.
 */

#include "kern/thread.h"
#include "kern/process.h"
#include "kern/vmspace.h"
#include "kern/kmem.h"
#include "kern/test-checkpoint.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

struct thread thread0;
static tid_t next_tid = 1;
static struct thread *reserved_tids;
static struct spinlock tid_registry_lock = {
	{ 0 }, LOCK_RANK_PROCESS, "thread ID registry", 0, 0
};
static struct thread *secondary_idle_threads;
static unsigned secondary_idle_count;

static int attach_thread(struct process *process, struct thread *thread);
static int reserve_tid(struct thread *thread);
static void release_tid(struct thread *thread);
static int prepare_created_thread(struct thread *thread);
static void kernel_thread_trampoline(void *argument);

/*
 * Finds the thread running on the current CPU, or none before threads exist.
 */
struct thread *
thread_current(
	void)
{
	hal_task_t task;

	/* There is no thread before the HAL task exists. */
	task = hal_task_get_current();
	if (task == NULL)
		return NULL;

	/* Reports the thread attached to the task. */
	return hal_task_get_private(task);
}

/*
 * Creates a user thread in a process.
 *
 * The thread starts at entry on the user stack once thread_start() runs.
 */
int
thread_create(
	struct process *process,
	uintptr_t entry,
	uintptr_t user_sp,
	struct thread **result)
{
	struct thread *thread;
	int error;

	/* Rejects a kernel process, bad user addresses, or a missing result. */
	if (process == NULL ||
	    process->vmspace == NULL ||
	    process->vmspace == &kernel_vmspace ||
	    !vmspace_user_range_valid(entry, 1) ||
	    !vmspace_user_range_valid(user_sp, 1) ||
	    result == NULL)
		return EINVAL;

	/* Allocates the thread and its identifier. */
	thread = kern_calloc(1, sizeof(*thread));
	if (thread == NULL)
		return ENOMEM;
	error = reserve_tid(thread);
	if (error != 0) {
		kern_free(thread);
		return error;
	}

	/* Fills the thread with the default scheduling parameters. */
	refcount_init(&thread->refs, 1);
	thread->proc = process;
	thread->state = THREAD_NEW;
	thread->signal_altstack_flags = SS_DISABLE;
	waitq_init(&thread->join_waitq, "thread join");
	thread->sched.priority = SCHED_PRIORITY_DEFAULT;
	thread->sched.quantum = SCHED_QUANTUM_TICKS;

	/* Creates the HAL task in the process address space. */
	thread->task = hal_task_create(process->vmspace->space,
		(void (*)(void *))entry, NULL, (void *)user_sp);
	if (thread->task == NULL) {
		release_tid(thread);
		kern_free(thread);
		return ENOMEM;
	}

	hal_task_set_private(thread->task, thread);

	/* Prepares the scheduler state and joins the process. */
	error = prepare_created_thread(thread);
	if (error != 0) {
		release_tid(thread);
		kern_free(thread);
		return error;
	}

	error = attach_thread(process, thread);
	if (error != 0) {
		hal_task_set_private(thread->task, NULL);
		hal_task_destroy(thread->task);
		release_tid(thread);
		kern_free(thread);
		return error;
	}

	*result = thread;

	/* Reports the created thread. */
	return 0;
}

/*
 * Wraps a forked HAL task in a new thread of the child process.
 */
int
thread_fork(
	struct process *process,
	hal_task_t task,
	struct thread **result)
{
	struct thread *thread;
	int error;

	/* Rejects a task that does not belong to the process address space. */
	if (process == NULL ||
	    process->vmspace == NULL ||
	    task == NULL ||
	    result == NULL ||
	    hal_task_get_space(task) != process->vmspace->space)
		return EINVAL;

	/* Allocates the thread and its identifier. */
	thread = kern_calloc(1, sizeof(*thread));
	if (thread == NULL)
		return ENOMEM;
	error = reserve_tid(thread);
	if (error != 0) {
		kern_free(thread);
		return error;
	}

	/* Fills the thread around the existing task. */
	refcount_init(&thread->refs, 1);
	thread->proc = process;
	thread->task = task;
	thread->state = THREAD_NEW;
	thread->signal_altstack_flags = SS_DISABLE;
	waitq_init(&thread->join_waitq, "thread join");
	thread->sched.priority = SCHED_PRIORITY_DEFAULT;
	thread->sched.quantum = SCHED_QUANTUM_TICKS;
	hal_task_set_private(task, thread);

	/* Prepares the scheduler state and joins the process. */
	error = prepare_created_thread(thread);
	if (error != 0) {
		release_tid(thread);
		kern_free(thread);
		return error;
	}

	error = attach_thread(process, thread);
	if (error != 0) {
		hal_task_set_private(thread->task, NULL);
		hal_task_destroy(thread->task);
		release_tid(thread);
		kern_free(thread);
		return error;
	}

	*result = thread;

	/* Reports the created thread. */
	return 0;
}

/*
 * Creates a kernel thread in the kernel process.
 *
 * The thread runs entry with arg once thread_start() runs and exits when
 * entry returns.
 */
int
kthread_create(
	void (*entry)(void *),
	void *arg,
	int priority,
	struct thread **result)
{
	struct thread *thread;
	int error;

	/* Rejects a missing entry or result, or a priority out of range. */
	if (entry == NULL ||
	    result == NULL ||
	    priority < SCHED_PRIOR_HIGH ||
	    priority > SCHED_PRIOR_LOW)
		return EINVAL;

	/* Allocates the thread and its identifier. */
	thread = kern_calloc(1, sizeof(*thread));
	if (thread == NULL)
		return ENOMEM;
	error = reserve_tid(thread);
	if (error != 0) {
		kern_free(thread);
		return error;
	}

	/* Fills the thread with the requested priority and entry. */
	refcount_init(&thread->refs, 1);
	thread->proc = &process0;
	thread->state = THREAD_NEW;
	thread->signal_altstack_flags = SS_DISABLE;
	waitq_init(&thread->join_waitq, "thread join");
	thread->sched.priority = priority;
	thread->sched.quantum = SCHED_QUANTUM_TICKS;
	thread->kernel_entry = entry;
	thread->kernel_arg = arg;

	/* Creates the HAL task in the kernel address space. */
	thread->task = hal_task_create(HAL_SPACE_SYS, kernel_thread_trampoline,
				       thread, NULL);
	if (thread->task == NULL) {
		release_tid(thread);
		kern_free(thread);
		return ENOMEM;
	}

	hal_task_set_private(thread->task, thread);

	/* Prepares the scheduler state and joins the kernel process. */
	error = prepare_created_thread(thread);
	if (error != 0) {
		release_tid(thread);
		kern_free(thread);
		return error;
	}

	(void)attach_thread(&process0, thread);

	*result = thread;

	/* Reports the created thread. */
	return 0;
}

/*
 * Allocates the idle threads of the secondary CPUs.
 */
int
thread_prepare_secondaries(
	unsigned cpu_count)
{
	/* A single CPU needs no secondary idle threads. */
	if (cpu_count <= 1U)
		return 0;

	/* A repeated call must agree on the CPU count. */
	if (secondary_idle_threads != NULL) {
		if (secondary_idle_count == cpu_count - 1U)
			return 0;
		return EBUSY;
	}

	/* Allocates one idle thread per secondary CPU. */
	secondary_idle_threads = kern_calloc(cpu_count - 1U,
	    sizeof(*secondary_idle_threads));
	if (secondary_idle_threads == NULL)
		return ENOMEM;
	secondary_idle_count = cpu_count - 1U;

	/* Reports the allocated threads. */
	return 0;
}

/*
 * Destroys a thread that was created but never started.
 */
int
thread_abort_new(
	struct thread *thread)
{
	struct process *process;
	struct thread **link;
	unsigned long irq;

	/* Rejects a missing thread, the current thread, or one without a process. */
	if (thread == NULL || thread == curthread)
		return EINVAL;
	process = thread->proc;
	if (process == NULL)
		return EINVAL;

	/* Only a thread that never ran can be aborted. */
	irq = spin_lock_irqsave(&process->lock);

	if (thread->state != THREAD_NEW) {
		spin_unlock_irqrestore(&process->lock, irq);
		return EBUSY;
	}

	/* Unlinks the thread from the process. */
	link = &process->threads;
	while (*link != NULL && *link != thread)
		link = &(*link)->proc_next;
	if (*link != thread) {
		spin_unlock_irqrestore(&process->lock, irq);
		return ESRCH;
	}

	*link = thread->proc_next;
	thread->proc_next = NULL;
	if (process->thread_count == 0)
		HAL_FATAL("new thread count underflow");
	process->thread_count--;

	/* A pending stop no longer waits for this thread. */
	if (process->stop_requested) {
		if (process->stop_target_count == 0)
			HAL_FATAL("new thread stop target underflow");
		process->stop_target_count--;
	}

	thread->state = THREAD_DEAD;

	spin_unlock_irqrestore(&process->lock, irq);

	/* Destroys the task and the thread. */
	hal_task_set_private(thread->task, NULL);
	hal_task_destroy(thread->task);
	thread->task = NULL;
	release_tid(thread);
	thread_release(thread);

	/* Reports the aborted thread. */
	return 0;
}

/*
 * Makes the current HAL task of a secondary CPU its idle thread.
 */
void
thread_init_secondary(
	hal_cpu_id_t cpu)
{
	struct thread *thread;
	hal_task_t task;

	/* The CPU must have a preallocated idle thread. */
	if (cpu == 0 ||
	    cpu > secondary_idle_count ||
	    secondary_idle_threads == NULL)
		HAL_FATAL("invalid secondary idle thread");
	thread = &secondary_idle_threads[cpu - 1U];

	/* The current task must not belong to a thread yet. */
	task = hal_task_get_current();
	if (task == NULL || hal_task_get_private(task) != NULL)
		HAL_FATAL("secondary HAL task ownership");

	/* Fills the idle thread, pinned to its CPU with a negative identifier. */
	thread->tid = -(tid_t)cpu;
	refcount_init(&thread->refs, 1);
	thread->proc = &process0;
	thread->task = task;
	thread->state = THREAD_RUNNING;
	thread->flags = THREAD_FLAG_IDLE;
	waitq_init(&thread->join_waitq, "idle join");
	thread->sched.priority = SCHED_PRIORITY_DEFAULT;
	thread->sched.quantum = SCHED_QUANTUM_TICKS;
	thread->sched.cpu = cpu;
	thread->sched.last_cpu = cpu;
	hal_task_set_private(task, thread);
}

/*
 * Joins the secondary idle threads to the kernel process.
 */
void
thread_attach_secondaries(
	void)
{
	struct thread *thread;
	unsigned index;

	/* This runs on the BSP before ordinary kernel threads are published. */
	for (index = 0; index < secondary_idle_count; index++) {
		thread = &secondary_idle_threads[index];
		if (thread->task == NULL || thread->proc_next != NULL)
			HAL_FATAL("secondary idle thread not initialized");
		(void)attach_thread(&process0, thread);
	}
}

/*
 * Makes a new thread runnable.
 */
void
thread_start(
	struct thread *thread)
{
	bool enabled;

	/* Only a thread that never ran can be started. */
	if (thread == NULL || thread->state != THREAD_NEW)
		return;

	/* Adds the thread to the scheduler with interrupts disabled. */
	enabled = hal_irq_disable();
	sched_add(thread);
	if (enabled)
		hal_irq_enable();
}

/*
 * Ends the current thread, and its process when it was the last thread.
 */
void
thread_exit(
	int status)
{
	struct thread *thread;

	/* Exiting needs a current thread. */
	thread = curthread;
	if (thread == NULL)
		HAL_FATAL("thread_exit without current thread");

	/* The last thread of a user process takes the process with it. */
	if (thread->proc != NULL && thread->proc != &process0)
		process_exit_if_last_thread(status);

	/* Hands the thread to the scheduler for retirement. */
	thread->exit_status = status;
	sched_exit_current();
}

/*
 * Retires an exiting thread once the scheduler has switched away from it.
 *
 * The thread becomes a zombie that a joiner or the reaper may claim; a
 * detached thread is reaped here.
 */
void
thread_sched_retired(
	struct thread *thread)
{
	struct process *process;
	struct thread *member;
	unsigned long irq;

	/* Only an exiting thread with a process can retire. */
	if (thread == NULL || thread->state != THREAD_EXITING) {
		HAL_FATAL("invalid retired thread");
		return;
	}

	process = thread->proc;
	if (process == NULL) {
		HAL_FATAL("retired thread without process");
		return;
	}

	/*
	 * EXITING cannot be reaped, so these references are acquired before
	 * the first ZOMBIE publication.  They are the retired hook's
	 * ownership: a remote join/reaper may claim and unlink the thread
	 * immediately after the store below, but neither the thread nor its
	 * process can be freed until all post-publication work in this
	 * function is complete.
	 */
	thread_ref(thread);
	process_ref(process);

	/* Publishes the zombie state. */
	irq = spin_lock_irqsave(&process->lock);

	atomic_raw_store_release((volatile unsigned *)&thread->state,
	    THREAD_ZOMBIE);
	thread->state_generation++;

	/*
	 * A stop generation counts live threads.  If a thread retires before
	 * it acknowledges that generation, it is no longer a target;
	 * otherwise all acknowledged waiters could sleep forever waiting for
	 * a dead task.
	 */
	if (process->stop_requested &&
	    !thread->exit_committed &&
	    thread->stop_generation != process->stop_generation) {
		if (process->stop_target_count == 0)
			HAL_FATAL("process stop target underflow");
		process->stop_target_count--;
		for (member = process->threads; member != NULL;
		     member = member->proc_next) {
			if (member != thread &&
			    member->state == THREAD_SLEEPING &&
			    member->stop_generation ==
			    process->stop_generation)
				sched_wakeup(member);
		}
	}

	waitq_wake_all(&thread->join_waitq);

	spin_unlock_irqrestore(&process->lock, irq);

	KERN_TEST_CHECKPOINT(KERN_TEST_THREAD_RETIRED_AFTER_PUBLISH, thread);

	/* Lets the process account for the retirement and reaps a detached thread. */
	process_thread_retired(thread);
	if (thread->detached)
		(void)thread_wait(thread, NULL);
	thread_release(thread);
	process_release(process);
}

/*
 * Reaps a zombie thread, reporting its exit status.
 *
 * Exactly one caller wins the zombie; EBUSY reports a thread that is not
 * a zombie or was already claimed.
 */
int
thread_wait(
	struct thread *thread,
	int *status)
{
	struct thread **link;
	unsigned expected;
	unsigned long irq;

	expected = THREAD_ZOMBIE;

	/* Rejects a missing thread or the current one. */
	if (thread == NULL || thread == curthread)
		return EINVAL;

	/* Claims the zombie. */
	if (!atomic_raw_compare_exchange((volatile unsigned *)&thread->state,
	    &expected, THREAD_REAPING))
		return EBUSY;
	if (status != NULL)
		*status = thread->exit_status;

	/* Destroys the task and unlinks the thread from its process. */
	hal_task_set_private(thread->task, NULL);
	hal_task_destroy(thread->task);
	irq = spin_lock_irqsave(&thread->proc->lock);

	link = &thread->proc->threads;
	while (*link != NULL && *link != thread)
		link = &(*link)->proc_next;
	if (*link == thread)
		*link = thread->proc_next;
	if (thread->proc->thread_count == 0)
		HAL_FATAL("thread count underflow");
	thread->proc->thread_count--;
	atomic_raw_store_release((volatile unsigned *)&thread->state,
	    THREAD_DEAD);

	spin_unlock_irqrestore(&thread->proc->lock, irq);

	/* Frees the identifier and the creator's reference. */
	release_tid(thread);
	thread_release(thread);

	/* Reports the reaped thread. */
	return 0;
}

/*
 * Takes a reference on a thread.
 */
void
thread_ref(
	struct thread *thread)
{
	/* Ignores a missing thread. */
	if (thread != NULL)
		refcount_get(&thread->refs);
}

/*
 * Drops a reference on a thread and frees a dead one with the last.
 */
void
thread_release(
	struct thread *thread)
{
	/* Only the last reference frees. */
	if (thread == NULL || !refcount_put(&thread->refs))
		return;

	/* A live or idle thread must never lose its last reference. */
	if (thread->state != THREAD_DEAD ||
	    (thread->flags & THREAD_FLAG_IDLE) != 0)
		HAL_FATAL("releasing live kernel thread");
	kern_free(thread);
}

/* Links a thread into a process that can still accept threads. */
static int
attach_thread(
	struct process *process,
	struct thread *thread)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&process->lock);

	/* A user process must be alive and not in the middle of an exec. */
	if (process != &process0 &&
	    process->state != PROCESS_NEW &&
	    process->state != PROCESS_RUNNING &&
	    process->state != PROCESS_STOPPED) {
		spin_unlock_irqrestore(&process->lock, irq);
		return ESRCH;
	}

	if (process->execing) {
		spin_unlock_irqrestore(&process->lock, irq);
		return EBUSY;
	}

	/* Links the thread; a pending stop now waits for it too. */
	thread->proc_next = process->threads;
	process->threads = thread;
	process->thread_count++;
	if (process->stop_requested)
		process->stop_target_count++;

	spin_unlock_irqrestore(&process->lock, irq);

	/* Reports the attached thread. */
	return 0;
}

/* Assigns the next free positive thread identifier. */
static int
reserve_tid(
	struct thread *thread)
{
	struct thread *candidate;
	unsigned long irq;
	tid_t assigned;
	int collision;

	/* Rejects a missing thread. */
	if (thread == NULL)
		return EINVAL;

	/* Searches from the next identifier, wrapping within the positives. */
	irq = spin_lock_irqsave(&tid_registry_lock);

	assigned = next_tid;
	for (;;) {
		collision = assigned <= 0;
		for (candidate = reserved_tids; !collision && candidate != NULL;
		    candidate = candidate->tid_next) {
			collision = candidate->tid == assigned;
		}

		if (!collision)
			break;
		if (assigned == INT32_MAX)
			assigned = 1;
		else
			assigned = assigned + 1;

		/* Every identifier is taken once the search comes around. */
		if (assigned == next_tid) {
			spin_unlock_irqrestore(&tid_registry_lock, irq);
			return EAGAIN;
		}
	}

	/* Records the identifier and registers the thread. */
	thread->tid = assigned;
	if (assigned == INT32_MAX)
		next_tid = 1;
	else
		next_tid = assigned + 1;
	thread->tid_next = reserved_tids;
	reserved_tids = thread;

	spin_unlock_irqrestore(&tid_registry_lock, irq);

	/* Reports the reserved identifier. */
	return 0;
}

/* Returns a thread identifier to the registry. */
static void
release_tid(
	struct thread *thread)
{
	struct thread **link;
	unsigned long irq;

	/* Idle threads and unregistered threads hold no identifier. */
	if (thread == NULL || thread->tid <= 0)
		return;

	/* Unlinks the thread from the registry. */
	irq = spin_lock_irqsave(&tid_registry_lock);

	link = &reserved_tids;
	while (*link != NULL && *link != thread)
		link = &(*link)->tid_next;
	if (*link == thread)
		*link = thread->tid_next;
	thread->tid_next = NULL;

	spin_unlock_irqrestore(&tid_registry_lock, irq);
}

/* Prepares the scheduler state of a new thread, destroying its task on failure. */
static int
prepare_created_thread(
	struct thread *thread)
{
	int error;

	/* A prepared thread keeps its task. */
	error = sched_prepare_thread(thread);
	if (error == 0)
		return 0;

	/* Destroys the task so that the caller only frees the thread. */
	hal_task_set_private(thread->task, NULL);
	hal_task_destroy(thread->task);
	thread->task = NULL;

	/* Reports the preparation failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Runs a kernel thread's entry function and exits when it returns. */
static void
kernel_thread_trampoline(
	void *argument)
{
	struct thread *thread;

	thread = argument;
	thread->kernel_entry(thread->kernel_arg);
	thread_exit(0);
}
