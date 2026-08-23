/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/thread.h"
#include "kern/process.h"
#include "kern/vmspace.h"
#include "kern/kmem.h"

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

struct thread *
thread_current(void)
{
	hal_task_t task = hal_task_get_current();
	return task != NULL ? hal_task_get_private(task) : NULL;
}

static int
attach_thread(struct process *process, struct thread *thread)
{
	unsigned long irq = spin_lock_irqsave(&process->lock);
	if (process != &process0 && process->state != PROCESS_NEW &&
	    process->state != PROCESS_RUNNING &&
	    process->state != PROCESS_STOPPED) {
		spin_unlock_irqrestore(&process->lock, irq);
		return ESRCH;
	}
	if (process->execing) {
		spin_unlock_irqrestore(&process->lock, irq);
		return EBUSY;
	}
	thread->proc_next = process->threads;
	process->threads = thread;
	process->thread_count++;
	if (process->stop_requested)
		process->stop_target_count++;
	spin_unlock_irqrestore(&process->lock, irq);
	return 0;
}

static int
reserve_tid(struct thread *thread)
{
	struct thread *candidate;
	unsigned long irq;
	tid_t assigned;

	if (thread == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&tid_registry_lock);
	assigned = next_tid;
	for (;;) {
		int collision = assigned <= 0;

		for (candidate = reserved_tids; !collision && candidate != NULL;
		    candidate = candidate->tid_next)
			collision = candidate->tid == assigned;
		if (!collision)
			break;
		assigned = assigned == INT32_MAX ? 1 : assigned + 1;
		if (assigned == next_tid) {
			spin_unlock_irqrestore(&tid_registry_lock, irq);
			return EAGAIN;
		}
	}
	thread->tid = assigned;
	next_tid = assigned == INT32_MAX ? 1 : assigned + 1;
	thread->tid_next = reserved_tids;
	reserved_tids = thread;
	spin_unlock_irqrestore(&tid_registry_lock, irq);
	return 0;
}

static void
release_tid(struct thread *thread)
{
	struct thread **link;
	unsigned long irq;

	if (thread == NULL || thread->tid <= 0)
		return;
	irq = spin_lock_irqsave(&tid_registry_lock);
	link = &reserved_tids;
	while (*link != NULL && *link != thread)
		link = &(*link)->tid_next;
	if (*link == thread)
		*link = thread->tid_next;
	thread->tid_next = NULL;
	spin_unlock_irqrestore(&tid_registry_lock, irq);
}

static int
prepare_created_thread(struct thread *thread)
{
	int error = sched_prepare_thread(thread);

	if (error == 0)
		return 0;
	hal_task_set_private(thread->task, NULL);
	hal_task_destroy(thread->task);
	thread->task = NULL;
	return error;
}

int
thread_create(struct process *process, uintptr_t entry, uintptr_t user_sp,
	      struct thread **result)
{
	struct thread *thread;

	if (process == NULL || process->vmspace == NULL ||
	    process->vmspace == &kernel_vmspace ||
	    !vmspace_user_range_valid(entry, 1) ||
	    !vmspace_user_range_valid(user_sp, 1) || result == NULL)
		return EINVAL;
	thread = kern_calloc(1, sizeof(*thread));
	if (thread == NULL)
		return ENOMEM;
	{
		int error = reserve_tid(thread);
		if (error != 0) {
			kern_free(thread);
			return error;
		}
	}
	refcount_init(&thread->refs, 1);
	thread->proc = process;
	thread->state = THREAD_NEW;
	thread->signal_altstack_flags = SS_DISABLE;
	waitq_init(&thread->join_waitq, "thread join");
	thread->sched.priority = SCHED_PRIORITY_DEFAULT;
	thread->sched.quantum = SCHED_QUANTUM_TICKS;
	thread->task = hal_task_create(process->vmspace->space,
		(void (*)(void *))entry, NULL, (void *)user_sp);
	if (thread->task == NULL) {
		release_tid(thread);
		kern_free(thread);
		return ENOMEM;
	}
	hal_task_set_private(thread->task, thread);
	{
		int error = prepare_created_thread(thread);
		if (error != 0) {
			release_tid(thread);
			kern_free(thread);
			return error;
		}
	}
	{
		int error = attach_thread(process, thread);
		if (error != 0) {
			hal_task_set_private(thread->task, NULL);
			hal_task_destroy(thread->task);
			release_tid(thread);
			kern_free(thread);
			return error;
		}
	}
	*result = thread;
	return 0;
}

int
thread_fork(struct process *process, hal_task_t task, struct thread **result)
{
	struct thread *thread;
	if (process == NULL || process->vmspace == NULL || task == NULL ||
	    result == NULL || hal_task_get_space(task) != process->vmspace->space)
		return EINVAL;
	thread = kern_calloc(1, sizeof(*thread));
	if (thread == NULL)
		return ENOMEM;
	{
		int error = reserve_tid(thread);
		if (error != 0) {
			kern_free(thread);
			return error;
		}
	}
	refcount_init(&thread->refs, 1);
	thread->proc = process;
	thread->task = task;
	thread->state = THREAD_NEW;
	thread->signal_altstack_flags = SS_DISABLE;
	waitq_init(&thread->join_waitq, "thread join");
	thread->sched.priority = SCHED_PRIORITY_DEFAULT;
	thread->sched.quantum = SCHED_QUANTUM_TICKS;
	hal_task_set_private(task, thread);
	{
		int error = prepare_created_thread(thread);
		if (error != 0) {
			release_tid(thread);
			kern_free(thread);
			return error;
		}
	}
	{
		int error = attach_thread(process, thread);
		if (error != 0) {
			hal_task_set_private(thread->task, NULL);
			hal_task_destroy(thread->task);
			release_tid(thread);
			kern_free(thread);
			return error;
		}
	}
	*result = thread;
	return 0;
}

static void
kernel_thread_trampoline(void *argument)
{
	struct thread *thread = argument;
	thread->kernel_entry(thread->kernel_arg);
	thread_exit(0);
}

int
kthread_create(void (*entry)(void *), void *arg, int priority,
	       struct thread **result)
{
	struct thread *thread;

	if (entry == NULL || result == NULL || priority < SCHED_PRIOR_HIGH ||
	    priority > SCHED_PRIOR_LOW)
		return EINVAL;
	thread = kern_calloc(1, sizeof(*thread));
	if (thread == NULL)
		return ENOMEM;
	{
		int error = reserve_tid(thread);
		if (error != 0) {
			kern_free(thread);
			return error;
		}
	}
	refcount_init(&thread->refs, 1);
	thread->proc = &process0;
	thread->state = THREAD_NEW;
	thread->signal_altstack_flags = SS_DISABLE;
	waitq_init(&thread->join_waitq, "thread join");
	thread->sched.priority = priority;
	thread->sched.quantum = SCHED_QUANTUM_TICKS;
	thread->kernel_entry = entry;
	thread->kernel_arg = arg;
	thread->task = hal_task_create(HAL_SPACE_SYS, kernel_thread_trampoline,
				       thread, NULL);
	if (thread->task == NULL) {
		release_tid(thread);
		kern_free(thread);
		return ENOMEM;
	}
	hal_task_set_private(thread->task, thread);
	{
		int error = prepare_created_thread(thread);
		if (error != 0) {
			release_tid(thread);
			kern_free(thread);
			return error;
		}
	}
	(void)attach_thread(&process0, thread);
	*result = thread;
	return 0;
}

int
thread_prepare_secondaries(unsigned cpu_count)
{
	if (cpu_count <= 1U)
		return 0;
	if (secondary_idle_threads != NULL)
		return secondary_idle_count == cpu_count - 1U ? 0 : EBUSY;
	secondary_idle_threads = kern_calloc(cpu_count - 1U,
	    sizeof(*secondary_idle_threads));
	if (secondary_idle_threads == NULL)
		return ENOMEM;
	secondary_idle_count = cpu_count - 1U;
	return 0;
}

void
thread_init_secondary(hal_cpu_id_t cpu)
{
	struct thread *thread;
	hal_task_t task;

	if (cpu == 0 || cpu > secondary_idle_count ||
	    secondary_idle_threads == NULL)
		HAL_FATAL("invalid secondary idle thread");
	thread = &secondary_idle_threads[cpu - 1U];
	task = hal_task_get_current();
	if (task == NULL || hal_task_get_private(task) != NULL)
		HAL_FATAL("secondary HAL task ownership");
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

void
thread_attach_secondaries(void)
{
	unsigned index;

	/* This runs on the BSP before ordinary kernel threads are published. */
	for (index = 0; index < secondary_idle_count; index++) {
		struct thread *thread = &secondary_idle_threads[index];
		if (thread->task == NULL || thread->proc_next != NULL)
			HAL_FATAL("secondary idle thread not initialized");
		(void)attach_thread(&process0, thread);
	}
}

void
thread_start(struct thread *thread)
{
	bool enabled;
	if (thread == NULL || thread->state != THREAD_NEW)
		return;
	enabled = hal_irq_disable();
	sched_add(thread);
	if (enabled)
		hal_irq_enable();
}

void
thread_exit(int status)
{
	struct thread *thread = curthread;
	if (thread == NULL)
		HAL_FATAL("thread_exit without current thread");
	if (thread->proc != NULL && thread->proc != &process0)
		process_exit_if_last_thread(status);
	thread->exit_status = status;
	sched_exit_current();
}

void
thread_sched_retired(struct thread *thread)
{
	struct thread *member;
	unsigned long irq;

	if (thread == NULL || thread->state != THREAD_EXITING)
		HAL_FATAL("invalid retired thread");
	thread->state = THREAD_ZOMBIE;
	thread->state_generation++;
	irq = spin_lock_irqsave(&thread->proc->lock);
	/* A stop generation counts live threads.  If a thread retires before it
	 * acknowledges that generation, it is no longer a target; otherwise all
	 * acknowledged waiters could sleep forever waiting for a dead task. */
	if (thread->proc->stop_requested &&
	    thread->stop_generation != thread->proc->stop_generation) {
		if (thread->proc->stop_target_count == 0)
			HAL_FATAL("process stop target underflow");
		thread->proc->stop_target_count--;
		for (member = thread->proc->threads; member != NULL;
		    member = member->proc_next)
			if (member != thread && member->state == THREAD_SLEEPING &&
			    member->stop_generation ==
			    thread->proc->stop_generation)
				sched_wakeup(member);
	}
	waitq_wake_all(&thread->join_waitq);
	spin_unlock_irqrestore(&thread->proc->lock, irq);
	process_thread_retired(thread);
	if (thread->detached) {
		thread_ref(thread);
		(void)thread_wait(thread, NULL);
		thread_release(thread);
	}
}

int
thread_wait(struct thread *thread, int *status)
{
	struct thread **link;
	unsigned expected = THREAD_ZOMBIE;
	unsigned long irq;
	if (thread == NULL || thread == curthread)
		return EINVAL;
	if (!atomic_raw_compare_exchange((volatile unsigned *)&thread->state,
	    &expected, THREAD_REAPING))
		return EBUSY;
	if (status != NULL)
		*status = thread->exit_status;
	hal_task_set_private(thread->task, NULL);
	hal_task_destroy(thread->task);
	irq = spin_lock_irqsave(&thread->proc->lock);
	link = &thread->proc->threads;
	while (*link != NULL && *link != thread)
		link = &(*link)->proc_next;
	if (*link == thread)
		*link = thread->proc_next;
	thread->proc->thread_count--;
	atomic_raw_store_release((volatile unsigned *)&thread->state,
	    THREAD_DEAD);
	spin_unlock_irqrestore(&thread->proc->lock, irq);
	release_tid(thread);
	thread_release(thread);
	return 0;
}

void
thread_ref(struct thread *thread)
{
	if (thread != NULL)
		refcount_get(&thread->refs);
}

void
thread_release(struct thread *thread)
{
	if (thread == NULL || !refcount_put(&thread->refs))
		return;
	if (thread->state != THREAD_DEAD ||
	    (thread->flags & THREAD_FLAG_IDLE) != 0)
		HAL_FATAL("releasing live kernel thread");
	kern_free(thread);
}
