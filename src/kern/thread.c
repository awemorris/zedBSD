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
static struct thread *secondary_idle_threads;
static unsigned secondary_idle_count;

struct thread *
thread_current(void)
{
	hal_task_t task = hal_task_get_current();
	return task != NULL ? hal_task_get_private(task) : NULL;
}

static void
attach_thread(struct process *process, struct thread *thread)
{
	unsigned long irq = spin_lock_irqsave(&process->lock);
	thread->proc_next = process->threads;
	process->threads = thread;
	process->thread_count++;
	spin_unlock_irqrestore(&process->lock, irq);
}

static tid_t
allocate_tid(void)
{
	return (tid_t)atomic_raw_fetch_add_relaxed(
	    (volatile unsigned *)&next_tid, 1U);
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
	thread->tid = allocate_tid();
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
		kern_free(thread);
		return ENOMEM;
	}
	hal_task_set_private(thread->task, thread);
	{
		int error = prepare_created_thread(thread);
		if (error != 0) {
			kern_free(thread);
			return error;
		}
	}
	attach_thread(process, thread);
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
	thread->tid = allocate_tid();
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
			kern_free(thread);
			return error;
		}
	}
	attach_thread(process, thread);
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
	thread->tid = allocate_tid();
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
		kern_free(thread);
		return ENOMEM;
	}
	hal_task_set_private(thread->task, thread);
	{
		int error = prepare_created_thread(thread);
		if (error != 0) {
			kern_free(thread);
			return error;
		}
	}
	attach_thread(&process0, thread);
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
		attach_thread(&process0, thread);
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
	thread->exit_status = status;
	sched_exit_current();
}

void
thread_sched_retired(struct thread *thread)
{
	if (thread == NULL || thread->state != THREAD_EXITING)
		HAL_FATAL("invalid retired thread");
	thread->state = THREAD_ZOMBIE;
	thread->state_generation++;
	{
		unsigned long irq = spin_lock_irqsave(&thread->proc->lock);
		waitq_wake_all(&thread->join_waitq);
		spin_unlock_irqrestore(&thread->proc->lock, irq);
	}
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
